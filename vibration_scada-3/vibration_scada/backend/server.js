const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const ModbusRTU = require('modbus-serial');
const cors = require('cors');
const fs = require('fs');
const path = require('path');
const readline = require('readline');

// --- TIMEOUT RACE HELPER ---
const promiseTimeout = (promise, ms) => {
  let timeout = new Promise((resolve, reject) => {
    let id = setTimeout(() => {
      clearTimeout(id);
      reject(new Error(`Connection timeout after ${ms}ms`));
    }, ms);
  });
  return Promise.race([promise, timeout]);
};

const app = express();
app.use(cors());
app.use(express.json());
const server = http.createServer(app);
const io = new Server(server, { cors: { origin: '*', methods: ['GET', 'POST'] } });

// --- DATA STORAGE PATHS ---
const configPath = path.join(__dirname, 'scada-config.json');
const historyPath = path.join(__dirname, 'scada_history.csv');
const alarmsPath = path.join(__dirname, 'scada_alarms.json');
const alarmRulesPath = path.join(__dirname, 'scada_alarm_rules.json');
const historianGroupsPath = path.join(__dirname, 'scada_historian_groups.json');

// --- INITIALIZE FILES ---
if (!fs.existsSync(historyPath)) {
  fs.writeFileSync(historyPath, "timestamp,gatewayId,nodeId,status,vibX,vibY,vibZ,rssi\n");
}
let alarms = [];
try { alarms = JSON.parse(fs.readFileSync(alarmsPath, 'utf8')); } catch (e) { alarms = []; }
let config = [];
try { config = JSON.parse(fs.readFileSync(configPath, 'utf8')); } catch (e) { config = []; }
let alarmRules = [];
try { alarmRules = JSON.parse(fs.readFileSync(alarmRulesPath, 'utf8')); } catch (e) { alarmRules = []; }
let historianGroups = [];
try { historianGroups = JSON.parse(fs.readFileSync(historianGroupsPath, 'utf8')); } catch (e) { historianGroups = []; }

const modbusClients = {};   
const gwStatus = {};        
const historyBuffer = {}; // Holds last 100 points per node for fast API responses

// --- ALARM LOGIC ---
function checkAlarm(nodeId, nodeName, type, message, level) {
  // Check if an unacknowledged alarm of this type already exists for this node
  const existing = alarms.find(a => a.nodeId === nodeId && a.type === type && !a.acknowledged);
  if (!existing) {
    const newAlarm = { id: Date.now().toString(), timestamp: new Date().toISOString(), nodeId, nodeName, type, message, level: level || 'critical', acknowledged: false };
    alarms.unshift(newAlarm);
    if (alarms.length > 500) alarms = alarms.slice(0, 500); // Keep last 500
    fs.writeFileSync(alarmsPath, JSON.stringify(alarms, null, 2));
    io.emit('alarmUpdate', newAlarm);
  }
}

async function pollGateways() {
  for (const gw of config) {
    if (!gw.ip || !gw.port) continue;

    if (!modbusClients[gw.id]) {
      const client = new ModbusRTU();
      try {
        // Wrap TCP connection in 3s timeout to prevent thread blocking
        await promiseTimeout(client.connectTCP(gw.ip, { port: parseInt(gw.port) }), 3000);
        client.setID(1);
        client.setTimeout(2000); // 2 second read timeout
        modbusClients[gw.id] = client;
        gwStatus[gw.id] = { connected: true, error: null, lastPoll: new Date().toISOString() };
      } catch (e) {
        modbusClients[gw.id] = null;
        gwStatus[gw.id] = { connected: false, error: e.message, lastPoll: new Date().toISOString() };
        io.emit('gatewayStatus', { id: gw.id, ...gwStatus[gw.id] });
        // Emit offline status for all nodes under this gateway
        for (const node of (gw.nodes || [])) {
          io.emit('sensorData', { gatewayId: gw.id, nodeId: node.id, connected: false, status: 0, vibX: 0, vibY: 0, vibZ: 0, rssi: 0 });
        }
        continue;
      }
    }

    const client = modbusClients[gw.id];
    let gwOk = true;
    for (const node of (gw.nodes || [])) {
      try {
        // Set unit ID per-node to support multi-device RS485 networks
        client.setID(Number(node.unitId || 1));

        const allRegs = [node.statusReg, node.vibXReg, node.vibYReg, node.vibZReg, node.rssiReg].map(Number);
        if (allRegs.some(isNaN)) {
          throw new Error("Invalid register configurations (NaN)");
        }
        const startReg = Math.min(...allRegs);
        const count = Math.max(...allRegs) - startReg + 1;
        if (count <= 0 || count > 100) {
          throw new Error(`Invalid register count range: ${count}`);
        }
        const data = await client.readHoldingRegisters(startReg, count);
        const get = (reg) => data.data[Number(reg) - startReg] ?? 0;

        const payload = {
          gatewayId: gw.id, nodeId: node.id, connected: true,
          status: get(node.statusReg),
          vibX: get(node.vibXReg), vibY: get(node.vibYReg), vibZ: get(node.vibZReg),
          rssi: get(node.rssiReg),
          lastUpdate: new Date().toISOString()
        };
        io.emit('sensorData', payload);

        // --- 1. HISTORICAL LOGGING (CSV & BUFFER) ---
        const activeTagsForThisNode = [];
        historianGroups.forEach(g => {
          if (g.active) {
            (g.variables || []).forEach(v => {
              if (v.nodeId === node.id) {
                activeTagsForThisNode.push(v.tag);
              }
            });
          }
        });

        if (activeTagsForThisNode.length > 0) {
          const a = {
            status: activeTagsForThisNode.includes('status'),
            vibX: activeTagsForThisNode.includes('vibX'),
            vibY: activeTagsForThisNode.includes('vibY'),
            vibZ: activeTagsForThisNode.includes('vibZ'),
            rssi: activeTagsForThisNode.includes('rssi')
          };
          const csvLine = `${payload.lastUpdate},${gw.id},${node.id},${a.status ? payload.status : ''},${a.vibX ? payload.vibX : ''},${a.vibY ? payload.vibY : ''},${a.vibZ ? payload.vibZ : ''},${a.rssi ? payload.rssi : ''}\n`;
          fs.appendFile(historyPath, csvLine, (err) => { if(err) console.error("CSV Write Error", err) });
          
          if (!historyBuffer[node.id]) historyBuffer[node.id] = [];
          historyBuffer[node.id].push({
            timestamp: payload.lastUpdate, gatewayId: gw.id, nodeId: node.id,
            status: a.status ? payload.status : null,
            vibX: a.vibX ? payload.vibX : null,
            vibY: a.vibY ? payload.vibY : null,
            vibZ: a.vibZ ? payload.vibZ : null,
            rssi: a.rssi ? payload.rssi : null
          });
          if (historyBuffer[node.id].length > 100) historyBuffer[node.id].shift();
        }

        // Auto-acknowledge system communication failure alarm once connection is healthy
        const commAlarmIndex = alarms.findIndex(a => a.nodeId === node.id && a.type === `COMM_FAIL_${node.id}` && !a.acknowledged);
        if (commAlarmIndex >= 0) {
          alarms[commAlarmIndex].acknowledged = true;
          fs.writeFileSync(alarmsPath, JSON.stringify(alarms, null, 2));
          io.emit('alarmUpdate', alarms[commAlarmIndex]);
        }

        // --- 2. ALARM GENERATION (CUSTOM RULES) ---
        alarmRules.forEach(rule => {
          if (rule.nodeId === node.id || rule.nodeId === 'ANY') {
            const val = Number(payload[rule.tag]);
            const targetVal = Number(rule.value);
            let triggered = false;
            if (rule.condition === '>' && val > targetVal) triggered = true;
            if (rule.condition === '<' && val < targetVal) triggered = true;
            if (rule.condition === '==' && val === targetVal) triggered = true;
            if (rule.condition === '>=' && val >= targetVal) triggered = true;
            if (rule.condition === '<=' && val <= targetVal) triggered = true;
            if (rule.condition === '!=' && val !== targetVal) triggered = true;

            if (triggered) {
              const ruleMsg = rule.message || `${rule.tag.toUpperCase()} is ${rule.condition} ${rule.value} (Actual: ${val})`;
              checkAlarm(node.id, node.name, `RULE_${rule.id}`, ruleMsg, rule.level);
            }
          }
        });

      } catch (e) {
        console.error(`Error polling node ${node.name || node.id}:`, e.message);
        // Generate system alarm for connection loss
        checkAlarm(node.id, node.name, `COMM_FAIL_${node.id}`, `Communication failure with node ${node.name || node.id}`, 'critical');
        
        // Mark all nodes in this gateway as offline since we are breaking the connection
        for (const n of (gw.nodes || [])) {
          io.emit('sensorData', { gatewayId: gw.id, nodeId: n.id, connected: false, status: 0, vibX: 0, vibY: 0, vibZ: 0, rssi: 0 });
        }
        try { client.close(); } catch (_) {}
        modbusClients[gw.id] = null;
        gwOk = false;
        break;
      }
    }

    gwStatus[gw.id] = { connected: gwOk, error: gwOk ? null : 'Read timeout/disconnect', lastPoll: new Date().toISOString() };
    io.emit('gatewayStatus', { id: gw.id, name: gw.name, ip: gw.ip, port: gw.port, ...gwStatus[gw.id] });
  }
  
  // Guarantee 3 seconds delay after the full cycle completes to avoid overlapping polling
  setTimeout(pollGateways, 3000);
}

// Start the continuous polling cycle
pollGateways();

// --- REST APIs FOR HISTORY & ALARMS ---

app.get('/api/alarms', (req, res) => res.json(alarms));

app.post('/api/alarms/ack/:id', (req, res) => {
  const alarm = alarms.find(a => a.id === req.params.id);
  if (alarm) {
    alarm.acknowledged = true;
    fs.writeFileSync(alarmsPath, JSON.stringify(alarms, null, 2));
    io.emit('alarmUpdate', alarm);
  }
  res.json({ success: true });
});

app.get('/api/alarm-rules', (req, res) => res.json(alarmRules));
app.post('/api/alarm-rules', (req, res) => {
  alarmRules = req.body;
  fs.writeFileSync(alarmRulesPath, JSON.stringify(alarmRules, null, 2));
  res.json({ success: true });
});

app.get('/api/historian-groups', (req, res) => res.json(historianGroups));
app.post('/api/historian-groups', (req, res) => {
  historianGroups = req.body;
  fs.writeFileSync(historianGroupsPath, JSON.stringify(historianGroups, null, 2));
  res.json({ success: true });
});

app.get('/api/history', (req, res) => {
  const { nodeId, from, to } = req.query;
  if (!nodeId) return res.json([]);

  // If no date filters are supplied, return the rolling memory buffer instantly
  if (!from && !to) {
    return res.json(historyBuffer[nodeId] || []);
  }

  if (!fs.existsSync(historyPath)) {
    return res.json([]);
  }

  try {
    const results = [];
    const rl = readline.createInterface({
      input: fs.createReadStream(historyPath),
      crlfDelay: Infinity
    });

    let isFirstLine = true;
    const fromTime = from ? new Date(from).getTime() : 0;
    const toTime = to ? new Date(to).getTime() : Infinity;

    rl.on('line', (line) => {
      if (isFirstLine) {
        isFirstLine = false;
        return;
      }
      
      const parts = line.split(',');
      if (parts.length < 8) return;

      const timestampStr = parts[0];
      const rowNodeId = parts[2];

      if (rowNodeId === nodeId) {
        const rowTime = new Date(timestampStr).getTime();
        if (rowTime >= fromTime && rowTime <= toTime) {
          results.push({
            timestamp: timestampStr,
            gatewayId: parts[1],
            nodeId: rowNodeId,
            status: parts[3] ? parseInt(parts[3]) : null,
            vibX: parts[4] ? parseFloat(parts[4]) : null,
            vibY: parts[5] ? parseFloat(parts[5]) : null,
            vibZ: parts[6] ? parseFloat(parts[6]) : null,
            rssi: parts[7] ? parseFloat(parts[7]) : null
          });
        }
      }
    });

    rl.on('close', () => {
      // Limit to last 500 matching data points to ensure frontend rendering performance
      const limited = results.slice(-500);
      res.json(limited);
    });

    rl.on('error', (err) => {
      console.error("History query stream error", err);
      if (!res.headersSent) {
        res.status(500).send("Error reading history");
      }
    });
  } catch (e) {
    console.error("History query error", e);
    if (!res.headersSent) {
      res.status(500).send("Error reading history");
    }
  }
});

app.get('/api/export', (req, res) => {
  const nodeId = req.query.nodeId;
  if (!fs.existsSync(historyPath)) {
    return res.status(404).send("No history file found");
  }

  if (!nodeId) {
    return res.download(historyPath, `scada_history_all.csv`);
  }
  
  try {
    res.header('Content-Type', 'text/csv');
    res.attachment(`scada_history_${nodeId}.csv`);
    
    const rl = readline.createInterface({
      input: fs.createReadStream(historyPath),
      crlfDelay: Infinity
    });
    
    let isFirstLine = true;
    rl.on('line', (line) => {
      if (isFirstLine) {
        res.write(line + '\n');
        isFirstLine = false;
      } else if (line.includes(`,${nodeId},`)) {
        res.write(line + '\n');
      }
    });
    
    rl.on('close', () => {
      res.end();
    });
    
    rl.on('error', (err) => {
      console.error("Export stream error", err);
      if (!res.headersSent) {
        res.status(500).send("Error generating export");
      }
    });
  } catch(e) {
    console.error("Export endpoint error", e);
    if (!res.headersSent) {
      res.status(500).send("Error generating export");
    }
  }
});


io.on('connection', (socket) => {
  socket.emit('configLoaded', config);
  for (const id in gwStatus) socket.emit('gatewayStatus', { id, ...gwStatus[id] });

  socket.on('updateConfig', (newConfig) => {
    config = newConfig;
    try { fs.writeFileSync(configPath, JSON.stringify(config, null, 2)); } catch (e) {}
    for (const id in modbusClients) {
      if (modbusClients[id]) try { modbusClients[id].close(); } catch (_) {}
      delete modbusClients[id];
    }
    socket.broadcast.emit('configLoaded', config);
  });
});

const PORT = process.env.PORT || 3001;

server.listen(PORT, () => console.log(`SCADA Backend with SQLite-free DB running on http://localhost:${PORT}`));
