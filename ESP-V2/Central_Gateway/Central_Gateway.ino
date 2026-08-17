#define HELTEC_POWER_BUTTON
#include <heltec_unofficial.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <ModbusIP_ESP8266.h>

// ===================== CONFIG =====================
const char* ssid       = "SCADA_GATEWAY";
const char* wifi_pass  = "adminpassword";
const char* admin_pass = "adminpassword";

WebServer server(80);
Preferences preferences;
ModbusIP mb;

// ===================== Modbus activity tracking =====================
// We detect a connected Modbus client by using a "heartbeat" register (reg 99).
// The ESP writes an incrementing counter to it every second.
// When a Modbus client is polling, mb.task() will process the TCP connection —
// we detect this by counting how many TCP clients are connected via mb.slave().
unsigned long lastMbPoll  = 0;
uint32_t      mbPollCount = 0;
uint16_t      mbHeartbeat = 0;

// ===================== NODE STORAGE =====================
#define MAX_NODES 10

struct Node {
  String id;
  String name;
  String status;
  float  value;
  int    battery;
  float  rssi;
  float  snr;
  unsigned long lastSeen;
  bool   active;
};

Node nodes[MAX_NODES];
int nodeCount = 0;

int findNode(const String& id) {
  for (int i = 0; i < nodeCount; i++)
    if (nodes[i].id == id) return i;
  return -1;
}

void saveNodes() {
  preferences.putInt("nodeCount", nodeCount);
  for (int i = 0; i < nodeCount; i++) {
    preferences.putString(("nid" + String(i)).c_str(), nodes[i].id);
    preferences.putString(("nnm" + String(i)).c_str(), nodes[i].name);
  }
}

void loadNodes() {
  nodeCount = 0;
  int n = preferences.getInt("nodeCount", 0);
  for (int i = 0; i < n && i < MAX_NODES; i++) {
    String id   = preferences.getString(("nid" + String(i)).c_str(), "");
    String name = preferences.getString(("nnm" + String(i)).c_str(), "");
    if (id.length() > 0) {
      nodes[nodeCount++] = {id, name.length() ? name : id, "OFFLINE", 0, 0, 0, 0, 0, false};
    }
  }
  if (nodeCount == 0) {
    nodes[0] = {"M1", "Machine M1", "OFFLINE", 0, 0, 0, 0, 0, false};
    nodeCount = 1;
    saveNodes();
  }
}

bool isRegistered(const String& id) { return findNode(id) >= 0; }

// ===================== LoRa =====================
volatile bool rxFlag = false;
void rx() { rxFlag = true; }

// ===================== forward declarations =====================
bool checkAdminCookie();

// ===================== API HANDLERS =====================

void handleMbStatus() {
  unsigned long ago = lastMbPoll ? (millis() - lastMbPoll) / 1000 : 9999;
  bool connected = (ago < 5);
  String json = "{\"connected\":"  + String(connected ? "true" : "false") +
                ",\"ago\":"        + String(ago) +
                ",\"polls\":"      + String(mbPollCount) + "}";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleApiData() {
  unsigned long now = millis();
  String json = "[";
  for (int i = 0; i < nodeCount; i++) {
    if (i) json += ",";
    unsigned long ageSec = nodes[i].lastSeen ? (now - nodes[i].lastSeen) / 1000 : 9999;
    json += "{";
    json += "\"id\":\""  + nodes[i].id     + "\",";
    json += "\"nm\":\""  + nodes[i].name   + "\",";
    json += "\"st\":\""  + nodes[i].status + "\",";
    json += "\"v\":"     + String(nodes[i].value, 2) + ",";
    json += "\"b\":"     + String(nodes[i].battery)  + ",";
    json += "\"rssi\":"  + String(nodes[i].rssi, 1)  + ",";
    json += "\"snr\":"   + String(nodes[i].snr,  1)  + ",";
    json += "\"age\":"   + String(ageSec);
    json += "}";
  }
  json += "]";
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.send(200, "application/json", json);
}

void handleAddNode() {
  if (!checkAdminCookie()) { server.send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
  String id   = server.arg("id");   id.trim();
  String name = server.arg("name"); name.trim();
  if (id.length() == 0)       { server.send(400, "application/json", "{\"error\":\"missing id\"}"); return; }
  if (isRegistered(id))        { server.send(200, "application/json", "{\"status\":\"already exists\"}"); return; }
  if (nodeCount >= MAX_NODES)  { server.send(400, "application/json", "{\"error\":\"max nodes reached\"}"); return; }
  if (name.length() == 0) name = "Machine " + id;
  nodes[nodeCount++] = {id, name, "OFFLINE", 0, 0, 0, 0, 0, false};
  saveNodes();
  server.send(200, "application/json", "{\"status\":\"ok\"}");
}

void handleDeleteNode() {
  if (!checkAdminCookie()) { server.send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
  String id  = server.arg("id");
  int    idx = findNode(id);
  if (idx < 0) { server.send(404, "application/json", "{\"error\":\"not found\"}"); return; }
  for (int i = idx; i < nodeCount - 1; i++) nodes[i] = nodes[i + 1];
  nodeCount--;
  saveNodes();
  server.send(200, "application/json", "{\"status\":\"deleted\"}");
}

void handleFactoryReset() {
  if (!checkAdminCookie()) { server.send(403, "application/json", "{\"error\":\"forbidden\"}"); return; }
  preferences.clear();
  server.send(200, "application/json", "{\"status\":\"reset\"}");
  delay(500);
  ESP.restart();
}

void handleLoginApi() {
  String pw = server.arg("pw");
  if (pw == String(admin_pass)) {
    server.sendHeader("Set-Cookie", "auth=1; Path=/; Max-Age=3600");
    server.send(200, "application/json", "{\"ok\":true}");
  } else {
    server.send(200, "application/json", "{\"ok\":false}");
  }
}

bool checkAdminCookie() {
  return server.header("Cookie").indexOf("auth=1") >= 0;
}

// ===================== HTML PAGES =====================

const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SCADA UNIFIDE</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:'Segoe UI',Arial,sans-serif;background:#f1f5f9;min-height:100vh}
    .topbar{background:#1e293b;color:white;padding:14px 24px;display:flex;justify-content:space-between;align-items:center}
    .topbar h1{font-size:18px;font-weight:600;letter-spacing:.5px}
    .cfg-btn{background:#3b82f6;color:white;border:none;padding:8px 18px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600}
    .cfg-btn:hover{background:#2563eb}
    .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(280px,1fr));gap:16px;padding:20px;max-width:1100px;margin:auto}
    .card{background:white;border-radius:10px;overflow:hidden;box-shadow:0 1px 4px rgba(0,0,0,.08)}
    .card-top{padding:12px 16px;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #f1f5f9}
    .card-top h2{font-size:15px;font-weight:600;color:#1e293b}
    .badge{font-size:11px;font-weight:700;padding:3px 10px;border-radius:20px;text-transform:uppercase;letter-spacing:.5px}
    .badge.NORMAL{background:#dcfce7;color:#166534}
    .badge.ALERTE{background:#fef9c3;color:#854d0e}
    .badge.OFFLINE{background:#fee2e2;color:#991b1b}
    .card-body{padding:10px 16px}
    .row{display:flex;justify-content:space-between;align-items:center;padding:7px 0;border-bottom:1px solid #f8fafc;font-size:14px}
    .row:last-child{border-bottom:none}
    .row span:first-child{color:#64748b}
    .row span:last-child{font-weight:600;font-family:monospace;color:#1e293b}
    .dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#10b981;margin-right:6px;animation:pulse 2s infinite}
    .dot.off{background:#ef4444;animation:none}
    @keyframes pulse{0%,100%{opacity:1}50%{opacity:.4}}
    .overlay{display:none;position:fixed;inset:0;background:rgba(0,0,0,.5);z-index:100;justify-content:center;align-items:center}
    .overlay.show{display:flex}
    .modal{background:white;border-radius:12px;padding:28px 32px;width:320px;box-shadow:0 20px 40px rgba(0,0,0,.2)}
    .modal h3{font-size:16px;font-weight:700;margin-bottom:16px;color:#1e293b}
    .modal input{width:100%;padding:10px 12px;border:1px solid #e2e8f0;border-radius:6px;font-size:14px;margin-bottom:12px;outline:none}
    .modal input:focus{border-color:#3b82f6}
    .modal-btns{display:flex;gap:8px;justify-content:flex-end;margin-top:4px}
    .btn-cancel{background:white;border:1px solid #e2e8f0;padding:8px 16px;border-radius:6px;cursor:pointer;font-size:14px}
    .btn-go{background:#3b82f6;color:white;border:none;padding:8px 16px;border-radius:6px;cursor:pointer;font-size:14px;font-weight:600}
    .err-msg{color:#dc2626;font-size:12px;margin-bottom:8px;display:none}
  </style>
</head>
<body>
  <div class="topbar">
    <h1>&#9881; Factory Floor Monitor</h1>
    <button class="cfg-btn" onclick="document.getElementById('ov').classList.add('show')">&#9881; Config</button>
  </div>
  <div class="grid" id="cards"></div>
  <div class="overlay" id="ov" onclick="if(event.target===this)closeModal()">
    <div class="modal">
      <h3>&#128274; Admin Access</h3>
      <p id="errmsg" class="err-msg">Wrong password. Try again.</p>
      <input type="password" id="pw" placeholder="Enter admin password" onkeydown="if(event.key==='Enter')login()">
      <div class="modal-btns">
        <button class="btn-cancel" onclick="closeModal()">Cancel</button>
        <button class="btn-go" onclick="login()">Enter</button>
      </div>
    </div>
  </div>
  <script>
    function closeModal(){document.getElementById('ov').classList.remove('show');document.getElementById('pw').value='';document.getElementById('errmsg').style.display='none'}
    function login(){
      var pw=document.getElementById('pw').value;
      fetch('/api/login',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'pw='+encodeURIComponent(pw)})
        .then(r=>r.json()).then(d=>{if(d.ok){window.location='/admin'}else{document.getElementById('errmsg').style.display='block'}});
    }
    setInterval(function(){
      fetch('/api/data').then(r=>r.json()).then(data=>{
        let html='';
        data.forEach(n=>{
          let off=n.age>5;
          let st=off?'OFFLINE':n.st;
          html+=`<div class="card">
            <div class="card-top">
              <h2><span class="dot${off?' off':''}"></span>${n.nm} (${n.id})</h2>
              <span class="badge ${st}">${st}</span>
            </div>
            <div class="card-body">
              <div class="row"><span>Vibration</span><span>${n.v} g</span></div>
              <div class="row"><span>Battery</span><span>${n.b}%</span></div>
            </div>
          </div>`;
        });
        document.getElementById('cards').innerHTML=html;
      });
    },1000);
  </script>
</body>
</html>
)rawliteral";

const char admin_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>SCADA Admin</title>
  <style>
    *{box-sizing:border-box;margin:0;padding:0}
    body{font-family:'Segoe UI',monospace;background:#0f172a;color:#cbd5e1;min-height:100vh}
    .topbar{background:#1e293b;padding:14px 24px;display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #334155}
    .topbar h1{font-size:17px;color:#f1f5f9;font-weight:600}
    a.back{color:#94a3b8;font-size:13px;text-decoration:none}
    a.back:hover{color:white}
    .container{max-width:1000px;margin:24px auto;padding:0 16px}
    .panel{background:#1e293b;border-radius:10px;padding:18px 20px;margin-bottom:18px;border:1px solid #334155}
    .panel h3{font-size:13px;color:#94a3b8;text-transform:uppercase;letter-spacing:.8px;margin-bottom:12px}
    .mb-row{display:flex;align-items:center;gap:10px;font-size:14px}
    .dot{width:10px;height:10px;border-radius:50%;background:#ef4444;flex-shrink:0;transition:background .3s}
    .dot.ok{background:#10b981}
    table{width:100%;border-collapse:collapse;font-size:13px}
    th{background:#334155;color:#94a3b8;font-weight:600;text-transform:uppercase;font-size:11px;letter-spacing:.6px;padding:10px 12px;text-align:left}
    td{padding:9px 12px;border-bottom:1px solid #0f172a;color:#e2e8f0}
    tr:last-child td{border-bottom:none}
    tr:hover td{background:#263348}
    .st-badge{font-size:11px;font-weight:700;padding:2px 9px;border-radius:20px;text-transform:uppercase}
    .st-NORMAL{background:#064e3b;color:#6ee7b7}
    .st-ALERTE{background:#451a03;color:#fcd34d}
    .st-OFFLINE{background:#450a0a;color:#fca5a5}
    .form-row{display:flex;gap:10px;flex-wrap:wrap;align-items:center}
    .form-row input{flex:1;min-width:120px;background:#0f172a;border:1px solid #334155;color:#e2e8f0;padding:9px 12px;border-radius:6px;font-size:13px;outline:none}
    .form-row input:focus{border-color:#3b82f6}
    .btn{padding:9px 18px;border:none;border-radius:6px;cursor:pointer;font-size:13px;font-weight:600}
    .btn-add{background:#3b82f6;color:white}
    .btn-add:hover{background:#2563eb}
    .btn-del{background:#7f1d1d;color:#fca5a5;padding:4px 10px;font-size:11px;border:none;border-radius:4px;cursor:pointer}
    .btn-del:hover{background:#991b1b}
    .btn-reset{background:#7f1d1d;color:#fca5a5}
    .btn-reset:hover{background:#991b1b}
    .footer-row{display:flex;justify-content:flex-end;margin-top:12px}
    .toast{position:fixed;bottom:24px;right:24px;background:#1e293b;border:1px solid #334155;color:#cbd5e1;padding:12px 20px;border-radius:8px;font-size:13px;display:none;z-index:99}
    .toast.show{display:block}
  </style>
</head>
<body>
  <div class="topbar">
    <h1>SCADA Network Admin</h1>
    <a href="/" class="back">&#8592; Operator View</a>
  </div>
  <div class="container">
    <div class="panel">
      <h3>Modbus TCP Server (S7-1200 Link)</h3>
      <div class="mb-row">
        <div class="dot" id="mbdot"></div>
        <span id="mbstatus">Checking...</span>
        <span id="mbpolls" style="margin-left:auto;color:#64748b;font-size:12px"></span>
      </div>
    </div>
    <div class="panel">
      <h3>Registered Nodes</h3>
      <table>
        <thead><tr><th>ID</th><th>Name</th><th>Status</th><th>Vibration</th><th>RSSI</th><th>SNR</th><th>MB Reg</th><th>Age</th><th></th></tr></thead>
        <tbody id="tbody"></tbody>
      </table>
    </div>
    <div class="panel">
      <h3>Register New Node</h3>
      <div class="form-row">
        <input type="text" id="newId"   placeholder="ID (e.g. M2)">
        <input type="text" id="newName" placeholder="Name (e.g. Pump B)">
        <button class="btn btn-add" onclick="addNode()">Register Node</button>
      </div>
    </div>
    <div class="footer-row">
      <button class="btn btn-reset" onclick="factoryReset()">&#9888; Factory Reset</button>
    </div>
  </div>
  <div class="toast" id="toast"></div>
  <script>
    function toast(msg){var t=document.getElementById('toast');t.textContent=msg;t.classList.add('show');setTimeout(()=>t.classList.remove('show'),2500)}
    function loadMb(){
      fetch('/api/mbstatus').then(r=>r.json()).then(d=>{
        var dot=document.getElementById('mbdot');
        var st=document.getElementById('mbstatus');
        var pl=document.getElementById('mbpolls');
        if(d.connected){
          dot.classList.add('ok');
          st.textContent='PLC Connected — polling active';
          st.style.color='#6ee7b7';
        } else {
          dot.classList.remove('ok');
          st.textContent=d.ago>999?'Waiting for PLC poll...':'Last poll: '+d.ago+'s ago';
          st.style.color='#94a3b8';
        }
        pl.textContent='Total polls: '+d.polls;
      });
    }
    function load(){
      fetch('/api/data').then(r=>r.json()).then(d=>{
        let html='';
        d.forEach((n,i)=>{
          let st=n.age>5?'OFFLINE':n.st;
          let mbReg=40101+(i*3);
          html+=`<tr>
            <td>${n.id}</td><td>${n.nm}</td>
            <td><span class="st-badge st-${st}">${st}</span></td>
            <td>${n.v} g</td><td>${n.rssi}</td><td>${n.snr}</td>
            <td>${mbReg}</td>
            <td>${n.age>999?'Never':n.age+'s ago'}</td>
            <td><button class="btn-del" onclick="delNode('${n.id}')">Remove</button></td>
          </tr>`;
        });
        document.getElementById('tbody').innerHTML=html;
      });
    }
    function addNode(){
      let id=document.getElementById('newId').value.trim();
      let nm=document.getElementById('newName').value.trim();
      if(!id){toast('Enter a node ID');return;}
      fetch('/api/addNode?id='+encodeURIComponent(id)+'&name='+encodeURIComponent(nm))
        .then(r=>r.json()).then(d=>{toast(d.status||d.error);document.getElementById('newId').value='';document.getElementById('newName').value='';load();});
    }
    function delNode(id){
      if(!confirm('Remove node '+id+'?'))return;
      fetch('/api/delNode?id='+encodeURIComponent(id)).then(r=>r.json()).then(()=>{toast('Removed '+id);load();});
    }
    function factoryReset(){
      if(!confirm('FACTORY RESET — deletes all nodes and restarts. Sure?'))return;
      if(!confirm('Last warning: all data will be lost. Continue?'))return;
      fetch('/api/factoryReset').then(()=>toast('Resetting...'));
    }
    setInterval(load,1000);
    setInterval(loadMb,2000);
    load(); loadMb();
  </script>
</body>
</html>
)rawliteral";

// ===================== SETUP =====================
void setup() {
  heltec_setup();
  preferences.begin("scada", false);
  loadNodes();

  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);
  radio.setFrequency(868.0);
  radio.setSpreadingFactor(7);
  radio.setSyncWord(RADIOLIB_SX126X_SYNC_WORD_PRIVATE);
  radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF);

  WiFi.softAP(ssid, wifi_pass);
  MDNS.begin("gateway-scada");

  const char* headers[] = {"Cookie"};
  server.collectHeaders(headers, 1);

  server.on("/", [](){server.send(200,"text/html",index_html);});
  server.on("/admin", [](){
    if (!checkAdminCookie()) {
      server.sendHeader("Location", "/");
      server.send(302, "text/plain", "");
      return;
    }
    server.send(200,"text/html",admin_html);
  });
  server.on("/api/login",        HTTP_POST, handleLoginApi);
  server.on("/api/data",                    handleApiData);
  server.on("/api/mbstatus",                handleMbStatus);
  server.on("/api/addNode",                 handleAddNode);
  server.on("/api/delNode",                 handleDeleteNode);
  server.on("/api/factoryReset",            handleFactoryReset);
  server.begin();

  // Modbus server — registers 100..129  (3 per node x 10 nodes)
  // Register 99 = heartbeat counter (incremented every second by ESP)
  mb.server();
  mb.addHreg(99, 0);  // heartbeat
  for (int i = 0; i < MAX_NODES * 3; i++) mb.addHreg(100 + i, 0);

  // Use a per-register callback on reg 99 to detect when a client reads it
  // Signature required by this library: uint16_t fn(TRegister* reg, uint16_t val)
  mb.onGet(HREG(99), [](TRegister* reg, uint16_t val) -> uint16_t {
    lastMbPoll = millis();
    mbPollCount++;
    return val;
  });
}

// ===================== LOOP =====================
unsigned long lastHeartbeat = 0;

void loop() {
  heltec_loop();
  server.handleClient();
  mb.task();

  // Update heartbeat register every second so Modbus Poll always sees changing data
  if (millis() - lastHeartbeat > 1000) {
    lastHeartbeat = millis();
    mbHeartbeat++;
    mb.Hreg(99, mbHeartbeat);
  }

  if (rxFlag) {
    rxFlag = false;
    String rxdata;
    if (radio.readData(rxdata) == RADIOLIB_ERR_NONE) {
      StaticJsonDocument<256> doc;
      if (!deserializeJson(doc, rxdata)) {
        String id  = doc["id"].as<String>();
        int    idx = findNode(id);
        if (idx >= 0) {
          nodes[idx].active   = true;
          nodes[idx].status   = doc["st"].as<String>();
          nodes[idx].value    = doc["v"].as<float>();
          nodes[idx].battery  = doc["b"].as<int>();
          nodes[idx].rssi     = radio.getRSSI();
          nodes[idx].snr      = radio.getSNR();
          nodes[idx].lastSeen = millis();

          int base = 100 + (idx * 3);
          mb.Hreg(base + 0, (nodes[idx].status == "NORMAL") ? 1 : 2);
          mb.Hreg(base + 1, (uint16_t)(nodes[idx].value * 100));
          mb.Hreg(base + 2, nodes[idx].battery);
        }
      }
    }
    radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF);
  }
}
