import { useState, useEffect } from 'react';
import io from 'socket.io-client';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer, Brush } from 'recharts';
import { LayoutDashboard, Settings, Radio, History, AlertTriangle, Plus, Trash2, Cpu, Server, Save, ArrowLeft, CheckCircle, XCircle, Download, Check, Bell, Activity } from 'lucide-react';
import './App.css';

const socket = io('http://localhost:3001');

const DEFAULT_NODE = { id: '', name: '', unitId: 1, statusReg: 100, vibXReg: 101, vibYReg: 102, vibZReg: 103, rssiReg: 104 };

export default function App() {
  const [tab, setTab] = useState('dashboard');
  const [gateways, setGateways] = useState([]);
  const [gwStatus, setGwStatus] = useState({});       
  const [sensorData, setSensorData] = useState({});   
  const [history, setHistory] = useState({});          
  const [selectedNode, setSelectedNode] = useState(null); 
  
  // Alarms State
  const [alarms, setAlarms] = useState([]);
  const [alarmRules, setAlarmRules] = useState([]);
  const [historianGroups, setHistorianGroups] = useState([]);

  // Historian State
  const [histSelection, setHistSelection] = useState(''); // combined node/group selection, e.g. "node-<id>" or "group-<id>"
  const [dbData, setDbData] = useState([]);
  const [histSelectedTags, setHistSelectedTags] = useState(['vibX', 'vibY', 'vibZ']);
  const [groupSelectionTags, setGroupSelectionTags] = useState([]);

  // Modal states
  const [showAddGw, setShowAddGw] = useState(false);
  const [showAddNode, setShowAddNode] = useState(false);
  const [showAddRule, setShowAddRule] = useState(false);
  const [showAddGroup, setShowAddGroup] = useState(false);
  
  const [editGw, setEditGw] = useState(null);
  const [editNode, setEditNode] = useState(null);
  const [editRule, setEditRule] = useState(null);
  const [editGroup, setEditGroup] = useState(null);
  const [targetGwId, setTargetGwId] = useState('');
  
  const [gwForm, setGwForm] = useState({ name: '', ip: '', port: 502 });
  const [nodeForm, setNodeForm] = useState(DEFAULT_NODE);
  const [ruleForm, setRuleForm] = useState({ nodeId: 'ANY', tag: 'vibX', condition: '>', value: 15, level: 'critical', message: '' });
  const [groupForm, setGroupForm] = useState({ name: '', variables: [] });

  // Historian Range Filters
  const [histFrom, setHistFrom] = useState('');
  const [histTo, setHistTo] = useState('');

  useEffect(() => {
    fetch('http://localhost:3001/api/alarms').then(r => r.json()).then(setAlarms).catch(console.error);
    fetch('http://localhost:3001/api/alarm-rules').then(r => r.json()).then(setAlarmRules).catch(console.error);
    fetch('http://localhost:3001/api/historian-groups').then(r => r.json()).then(setHistorianGroups).catch(console.error);

    socket.on('configLoaded', (cfg) => { if (cfg) setGateways(cfg); });
    socket.on('gatewayStatus', (s) => setGwStatus(prev => ({ ...prev, [s.id]: s })));
    socket.on('alarmUpdate', (newAlarm) => {
      setAlarms(prev => {
        const idx = prev.findIndex(a => a.id === newAlarm.id);
        if (idx >= 0) { const copy = [...prev]; copy[idx] = newAlarm; return copy; }
        return [newAlarm, ...prev].slice(0, 500);
      });
    });
    socket.on('sensorData', (d) => {
      const key = `${d.gatewayId}-${d.nodeId}`;
      setSensorData(prev => ({ ...prev, [key]: d }));
      if (d.connected) {
        setHistory(prev => {
          const arr = [...(prev[key] || []), { ...d, t: new Date().toLocaleTimeString() }];
          return { ...prev, [key]: arr.slice(-200) }; // Keep 200 points (~10 mins at 3s polling)
        });
      }
    });
    return () => { socket.off('configLoaded'); socket.off('gatewayStatus'); socket.off('sensorData'); socket.off('alarmUpdate'); };
  }, []);

  // Pre-populate historical chart values on node selection
  useEffect(() => {
    if (selectedNode) {
      const { gw, node } = selectedNode;
      const key = `${gw.id}-${node.id}`;
      fetch(`http://localhost:3001/api/history?nodeId=${node.id}`)
        .then(r => r.json())
        .then(data => {
          if (Array.isArray(data)) {
            const formatted = data.map(d => ({
              ...d,
              t: new Date(d.timestamp).toLocaleTimeString()
            }));
            setHistory(prev => ({ ...prev, [key]: formatted.slice(-200) }));
          }
        })
        .catch(console.error);
    }
  }, [selectedNode]);

  const ackAlarm = (id) => fetch(`http://localhost:3001/api/alarms/ack/${id}`, { method: 'POST' });
  
  const downloadCSV = () => {
    if (!histSelection) return;
    if (histSelection.startsWith('group-')) {
      window.open('http://localhost:3001/api/export', '_blank');
    } else {
      const nodeId = histSelection.replace('node-', '');
      window.open(`http://localhost:3001/api/export?nodeId=${nodeId}`, '_blank');
    }
  };

  const queryHistorian = () => {
    if (!histSelection) return;
    
    const fromParam = histFrom ? `&from=${histFrom}` : '';
    const toParam = histTo ? `&to=${histTo}` : '';

    if (histSelection.startsWith('group-')) {
      const gId = histSelection.replace('group-', '');
      const group = historianGroups.find(g => g.id === gId);
      if (!group || !group.variables || group.variables.length === 0) {
        setDbData([]);
        return;
      }
      
      const uniqueNodeIds = [...new Set(group.variables.map(v => v.nodeId))];
      const fetches = uniqueNodeIds.map(nodeId => 
        fetch(`http://localhost:3001/api/history?nodeId=${nodeId}${fromParam}${toParam}`)
          .then(r => r.json())
          .catch(() => [])
      );

      Promise.all(fetches).then(results => {
        const combined = {};
        
        results.forEach((nodeData, idx) => {
          const nodeId = uniqueNodeIds[idx];
          const vars = group.variables.filter(v => v.nodeId === nodeId);
          
          nodeData.forEach(row => {
            if (!row.timestamp) return;
            const roundedMs = Math.round(new Date(row.timestamp).getTime() / 3000) * 3000;
            const timeKey = new Date(roundedMs).toISOString();
            const localTimeStr = new Date(roundedMs).toLocaleString();
            
            if (!combined[timeKey]) {
              combined[timeKey] = { t: localTimeStr, timestamp: timeKey };
            }
            
            vars.forEach(v => {
              const label = `${v.nodeName} - ${v.tag.toUpperCase()}`;
              combined[timeKey][label] = row[v.tag] !== null ? parseFloat(row[v.tag]) : null;
            });
          });
        });

        const sorted = Object.values(combined).sort((a, b) => new Date(a.timestamp) - new Date(b.timestamp));
        setDbData(sorted);
      }).catch(() => setDbData([]));
      
    } else {
      const nodeId = histSelection.replace('node-', '');
      const url = `http://localhost:3001/api/history?nodeId=${nodeId}${fromParam}${toParam}`;
      
      fetch(url)
        .then(r => r.json())
        .then(data => {
          const formatted = data.map(d => ({
            t: new Date(d.timestamp).toLocaleString(),
            timestamp: d.timestamp,
            vibX: d.vibX !== null ? parseFloat(d.vibX) : null,
            vibY: d.vibY !== null ? parseFloat(d.vibY) : null,
            vibZ: d.vibZ !== null ? parseFloat(d.vibZ) : null,
            rssi: d.rssi !== null ? parseFloat(d.rssi) : null,
            status: d.status !== null ? parseInt(d.status) : null
          }));
          setDbData(formatted);
        }).catch(() => setDbData([]));
    }
  };

  const handleHistSelectionChange = (val) => {
    setHistSelection(val);
    if (val.startsWith('group-')) {
      const gId = val.replace('group-', '');
      const group = historianGroups.find(g => g.id === gId);
      if (group && group.variables) {
        const labels = group.variables.map(v => `${v.nodeName} - ${v.tag.toUpperCase()}`);
        setGroupSelectionTags(labels);
      } else {
        setGroupSelectionTags([]);
      }
    } else {
      setGroupSelectionTags([]);
    }
  };

  useEffect(() => {
    if (tab === 'historian' && histSelection) {
      // eslint-disable-next-line react-hooks/set-state-in-effect
      queryHistorian();
    }
    // eslint-disable-next-line react-hooks/exhaustive-deps
  }, [tab, histSelection]);

  // ---- CRUD HANDLERS ----
  const saveGateway = () => {
    let newGws;
    if (editGw) newGws = gateways.map(g => g.id === editGw.id ? { ...g, ...gwForm } : g);
    else newGws = [...gateways, { ...gwForm, id: Date.now().toString(), nodes: [] }];
    setGateways(newGws);
    socket.emit('updateConfig', newGws);
    setShowAddGw(false); setEditGw(null); setGwForm({ name: '', ip: '', port: 502 });
  };
  const deleteGateway = (id) => {
    const newGws = gateways.filter(g => g.id !== id);
    setGateways(newGws);
    socket.emit('updateConfig', newGws);
  };
  const openEditGw = (gw) => { setGwForm({ name: gw.name, ip: gw.ip, port: gw.port }); setEditGw(gw); setShowAddGw(true); };

  const saveNode = () => {
    const newGws = gateways.map(gw => {
      if (gw.id !== targetGwId) return gw;
      if (editNode) return { ...gw, nodes: gw.nodes.map(n => n.id === editNode.id ? { ...n, ...nodeForm } : n) };
      return { ...gw, nodes: [...gw.nodes, { ...nodeForm, id: Date.now().toString() }] };
    });
    setGateways(newGws);
    socket.emit('updateConfig', newGws);
    setShowAddNode(false); setEditNode(null); setNodeForm(DEFAULT_NODE);
  };
  const deleteNode = (gwId, nodeId) => {
    const newGws = gateways.map(gw => gw.id === gwId ? { ...gw, nodes: gw.nodes.filter(n => n.id !== nodeId) } : gw);
    setGateways(newGws);
    socket.emit('updateConfig', newGws);
  };
  const openAddNode = (gwId) => { setTargetGwId(gwId); setNodeForm(DEFAULT_NODE); setEditNode(null); setShowAddNode(true); };
  const openEditNode = (gwId, node) => { setTargetGwId(gwId); setNodeForm({ ...node, unitId: node.unitId ?? 1 }); setEditNode(node); setShowAddNode(true); };

  const saveRule = () => {
    let newRules;
    if (editRule) {
      newRules = alarmRules.map(r => r.id === editRule.id ? { ...r, ...ruleForm } : r);
    } else {
      newRules = [...alarmRules, { ...ruleForm, id: Date.now().toString() }];
    }
    setAlarmRules(newRules);
    fetch('http://localhost:3001/api/alarm-rules', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(newRules) });
    setShowAddRule(false);
    setEditRule(null);
  };
  const deleteRule = (id) => {
    const newRules = alarmRules.filter(r => r.id !== id);
    setAlarmRules(newRules);
    fetch('http://localhost:3001/api/alarm-rules', { method: 'POST', headers: {'Content-Type':'application/json'}, body: JSON.stringify(newRules) });
  };
  const openEditRule = (rule) => {
    setRuleForm({
      nodeId: rule.nodeId,
      tag: rule.tag,
      condition: rule.condition,
      value: rule.value,
      level: rule.level,
      message: rule.message || ''
    });
    setEditRule(rule);
    setShowAddRule(true);
  };

  const saveHistorianGroup = () => {
    let newGroups;
    if (editGroup) {
      newGroups = historianGroups.map(g => g.id === editGroup.id ? { ...g, ...groupForm } : g);
    } else {
      newGroups = [...historianGroups, { ...groupForm, id: Date.now().toString(), active: false }];
    }
    setHistorianGroups(newGroups);
    fetch('http://localhost:3001/api/historian-groups', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(newGroups)
    });
    setShowAddGroup(false);
    setEditGroup(null);
    setGroupForm({ name: '', variables: [] });
  };

  const deleteHistorianGroup = (id) => {
    const newGroups = historianGroups.filter(g => g.id !== id);
    setHistorianGroups(newGroups);
    fetch('http://localhost:3001/api/historian-groups', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(newGroups)
    });
  };

  const toggleHistorianGroupActive = (group) => {
    const newGroups = historianGroups.map(g => g.id === group.id ? { ...g, active: !g.active } : g);
    setHistorianGroups(newGroups);
    fetch('http://localhost:3001/api/historian-groups', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(newGroups)
    });
  };

  const openEditGroup = (group) => {
    setGroupForm({
      name: group.name,
      variables: group.variables || []
    });
    setEditGroup(group);
    setShowAddGroup(true);
  };

  const unacknowledgedAlarms = alarms.filter(a => !a.acknowledged);
  const allNodes = gateways.flatMap(gw => gw.nodes.map(n => ({ ...n, gwName: gw.name })));

  // ---- RENDER VIEWS ----
  const renderDashboard = () => {
    if (selectedNode) {
      const { gw, node } = selectedNode;
      const key = `${gw.id}-${node.id}`;
      const live = sensorData[key] || { vibX: 0, vibY: 0, vibZ: 0, rssi: 0, status: 0, connected: false };
      const hist = history[key] || [];
      return (
        <div className="node-detail-page">
          <div className="detail-topbar">
            <button className="btn btn-ghost" onClick={() => setSelectedNode(null)}><ArrowLeft size={16}/> Back</button>
            <div className="breadcrumb"><span className="bc-gw">{gw.name}</span><span className="bc-sep">/</span><span>{node.name}</span></div>
            <div className={`comm-badge ${live.connected ? 'comm-ok' : 'comm-err'}`}>{live.connected ? <CheckCircle size={14}/> : <XCircle size={14}/>} {live.connected ? 'LIVE' : 'NO DATA'}</div>
          </div>
          <div className="kpi-row">
            {[
              { label: 'Vibration X', val: live.vibX, unit: 'g', cls: 'axis-x' },
              { label: 'Vibration Y', val: live.vibY, unit: 'g', cls: 'axis-y' },
              { label: 'Vibration Z', val: live.vibZ, unit: 'g', cls: 'axis-z' },
              { label: 'Status', val: live.connected ? (live.status === 1 || live.status === 0 ? 'Normal' : 'Fault') : 'Offline', unit: '', cls: live.connected ? (live.status === 1 || live.status === 0 ? 'axis-status-ok' : 'axis-status-err') : 'axis-rssi' },
              { label: 'RSSI', val: live.rssi, unit: '', cls: 'axis-rssi' }
            ].map(m => (
              <div key={m.label} className={`metric-card ${m.cls}`}><div className="metric-label">{m.label}</div><div className="metric-value">{m.val}<span className="metric-unit"> {m.unit}</span></div></div>
            ))}
          </div>
          <div className="chart-panel">
            <div className="chart-panel-header">
              <div className="chart-panel-title">Live Spectral Analysis — {node.name}</div>
              <div className="chart-legend"><span className="legend-dot"><span className="ldot ldot-x"></span> X</span><span className="legend-dot"><span className="ldot ldot-y"></span> Y</span><span className="legend-dot"><span className="ldot ldot-z"></span> Z</span></div>
            </div>
            <div style={{ flex: 1, minHeight: 0 }}>
              {hist.length > 0 ? (
                <ResponsiveContainer width="100%" height={340}>
                  <LineChart data={hist} margin={{ top: 5, right: 20, left: -20, bottom: 5 }}>
                    <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,255,255,0.06)" />
                    <XAxis dataKey="t" stroke="#5a6a85" fontSize={11} />
                    <YAxis stroke="#5a6a85" fontSize={11} />
                    <Tooltip contentStyle={{ background: '#0b1120', border: '1px solid rgba(255,255,255,0.1)', borderRadius: '8px' }} />
                    <Line type="monotone" dataKey="vibX" stroke="#ff0055" strokeWidth={2} dot={false} isAnimationActive={false} />
                    <Line type="monotone" dataKey="vibY" stroke="#00f0ff" strokeWidth={2} dot={false} isAnimationActive={false} />
                    <Line type="monotone" dataKey="vibZ" stroke="#d000ff" strokeWidth={2} dot={false} isAnimationActive={false} />
                    <Brush dataKey="t" height={30} stroke="#00f0ff" fill="#060a14" />
                  </LineChart>
                </ResponsiveContainer>
              ) : <div className="empty-chart">Waiting for data from {gw.ip}:{gw.port}…</div>}
            </div>
          </div>
        </div>
      );
    }

    return (
      <div>
        {gateways.length === 0 && <div className="empty-chart" style={{ height: 200 }}>No gateways configured. Go to Configuration.</div>}
        {gateways.map(gw => {
          const st = gwStatus[gw.id];
          return (
            <div key={gw.id} className="gateway-block">
              <div className="gateway-block-header">
                <div className="gateway-name"><div className={`gw-dot ${st?.connected ? 'gw-dot-ok' : 'gw-dot-err'}`}></div><Server size={16} color="#00e5c0" /> {gw.name} <span className="gateway-meta">{gw.ip}:{gw.port}</span></div>
                <span className={`status-pill ${st?.connected ? 'ok' : 'fault'}`}>{st ? (st.connected ? 'TCP Connected' : `Offline — ${st.error || ''}`) : 'Not polled yet'}</span>
              </div>
              <table className="node-table">
                <thead><tr><th>Node</th><th>Comm</th><th>Status</th><th>Vib X</th><th>Vib Y</th><th>Vib Z</th><th>RSSI</th></tr></thead>
                <tbody>
                  {gw.nodes.map(node => {
                    const d = sensorData[`${gw.id}-${node.id}`];
                    return (
                      <tr key={node.id} onClick={() => setSelectedNode({ gw, node })} style={{ cursor: 'pointer' }}>
                        <td><div style={{ display: 'flex', alignItems: 'center', gap: 8 }}><Cpu size={13}/> {node.name}</div></td>
                        <td>{d ? (d.connected ? <span className="status-pill ok">Live</span> : <span className="status-pill fault">No Data</span>) : <span className="status-pill offline">—</span>}</td>
                        <td>{d?.connected ? (d.status === 1 || d.status === 0 ? <span className="status-pill ok">Normal</span> : <span className="status-pill fault">Fault</span>) : '—'}</td>
                        <td className="mono">{d?.vibX ?? '—'}</td><td className="mono">{d?.vibY ?? '—'}</td><td className="mono">{d?.vibZ ?? '—'}</td><td className="mono">{d?.rssi ?? '—'}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          );
        })}
      </div>
    );
  };

  const renderConfiguration = () => (
    <div>
      <div className="config-header">
        <h2>Network Configuration</h2>
        <button className="btn btn-primary" onClick={() => { setGwForm({ name: '', ip: '', port: 502 }); setEditGw(null); setShowAddGw(true); }}><Plus size={15}/> Add Gateway</button>
      </div>
      {gateways.map(gw => (
        <div className="gateway-config-card" key={gw.id}>
          <div className="gw-card-header">
            <div className="gw-card-title"><Server size={16}/> {gw.name}</div>
            <div className="gw-card-actions">
              <button className="btn btn-ghost btn-sm" onClick={() => openEditGw(gw)}>✏️ Edit</button>
              <button className="btn btn-ghost btn-sm" onClick={() => openAddNode(gw.id)}><Plus size={13}/> Node</button>
              <button className="btn btn-danger btn-sm" onClick={() => deleteGateway(gw.id)}><Trash2 size={13}/></button>
            </div>
          </div>
          <table className="node-config-table">
            <thead><tr><th>Node Name</th><th>Unit ID</th><th>Regs (Status/X/Y/Z)</th><th>Actions</th></tr></thead>
            <tbody>
              {gw.nodes.map(n => (
                <tr key={n.id}>
                  <td>{n.name}</td>
                  <td className="mono">{n.unitId || 1}</td>
                  <td className="mono">{n.statusReg} / {n.vibXReg} / {n.vibYReg} / {n.vibZReg}</td>
                  <td style={{ display: 'flex', gap: 6 }}><button className="btn btn-ghost btn-sm" onClick={() => openEditNode(gw.id, n)}>✏️</button><button className="btn btn-danger btn-sm" onClick={() => deleteNode(gw.id, n.id)}><Trash2 size={12}/></button></td>
                </tr>
              ))}
            </tbody>
          </table>
        </div>
      ))}

      {/* ALARM RULES CONFIGURATION */}
      <div style={{ marginTop: '3rem' }}>
          <div className="config-header" style={{ marginBottom: '1rem' }}>
              <h2>Alarm Rules Configuration</h2>
              <button className="btn btn-primary" onClick={() => { setRuleForm({ nodeId: 'ANY', tag: 'vibX', condition: '>', value: 15, level: 'critical', message: '' }); setEditRule(null); setShowAddRule(true); }}><Plus size={15}/> Add Alarm Rule</button>
          </div>
          <div className="gateway-block">
              <table className="node-table">
                  <thead><tr><th>Target Node</th><th>Tag</th><th>Condition</th><th>Value</th><th>Level</th><th>Actions</th></tr></thead>
                  <tbody>
                      {alarmRules.map(r => {
                          const nodeName = r.nodeId === 'ANY' ? 'Any Node' : (allNodes.find(n => n.id === r.nodeId)?.name || r.nodeId);
                          const tagNames = { vibX: 'Vibration X', vibY: 'Vibration Y', vibZ: 'Vibration Z', rssi: 'RSSI', status: 'Status Code' };
                          return (
                              <tr key={r.id}>
                                  <td>{nodeName}</td>
                                  <td>{tagNames[r.tag] || r.tag}</td>
                                  <td className="mono" style={{fontWeight: 'bold'}}>{r.condition}</td>
                                  <td className="mono">{r.value}</td>
                                  <td>
                                      <span className={`status-pill ${r.level === 'warning' ? 'warn' : 'fault'}`} style={{ background: r.level === 'warning' ? 'rgba(255,170,0,0.1)' : '', color: r.level === 'warning' ? '#ffaa00' : '' }}>
                                          {r.level.toUpperCase()}
                                      </span>
                                  </td>
                                  <td style={{ display: 'flex', gap: 6 }}>
                                      <button className="btn btn-ghost btn-sm" onClick={() => openEditRule(r)}>✏️ Edit</button>
                                      <button className="btn btn-danger btn-sm" onClick={() => deleteRule(r.id)}><Trash2 size={12}/></button>
                                  </td>
                              </tr>
                          );
                      })}
                      {alarmRules.length === 0 && <tr><td colSpan="6" style={{ textAlign: 'center', padding: '2rem', color: '#5a6a85' }}>No alarm rules configured.</td></tr>}
                  </tbody>
              </table>
          </div>
      </div>

      {/* HISTORIAN GROUPS CONFIGURATION */}
      <div style={{ marginTop: '3rem' }}>
          <div className="config-header" style={{ marginBottom: '1rem' }}>
              <h2>Historian Groups Configuration</h2>
              <button className="btn btn-primary" onClick={() => { setGroupForm({ name: '', variables: [] }); setEditGroup(null); setShowAddGroup(true); }}><Plus size={15}/> Create Group</button>
          </div>
          <div className="gateway-block">
              <table className="node-table">
                  <thead><tr><th>Group Name</th><th>Logging State</th><th>Variables</th><th>Actions</th></tr></thead>
                  <tbody>
                      {historianGroups.map(g => {
                          const varList = (g.variables || []).map(v => `${v.nodeName} (${v.tag.toUpperCase()})`).join(', ');
                          return (
                              <tr key={g.id}>
                                  <td><strong>{g.name}</strong></td>
                                  <td>
                                      <button 
                                          className={`btn btn-sm`} 
                                          style={{ background: g.active ? 'rgba(255, 61, 113, 0.15)' : 'rgba(0, 229, 192, 0.15)', color: g.active ? '#ff3d71' : '#00e5c0', border: `1px solid ${g.active ? '#ff3d71' : '#00e5c0'}`, minWidth: '130px', cursor: 'pointer' }}
                                          onClick={() => toggleHistorianGroupActive(g)}
                                      >
                                          {g.active ? '⏹ Stop Logging' : '▶ Start Logging'}
                                      </button>
                                  </td>
                                  <td style={{ color: '#e8eaf0', fontSize: '0.85rem' }}>{varList || <span style={{color: '#5a6a85', fontStyle: 'italic'}}>No variables configured</span>}</td>
                                  <td style={{ display: 'flex', gap: 6 }}>
                                      <button className="btn btn-ghost btn-sm" onClick={() => openEditGroup(g)}>✏️ Edit</button>
                                      <button className="btn btn-danger btn-sm" onClick={() => deleteHistorianGroup(g.id)}><Trash2 size={12}/></button>
                                  </td>
                              </tr>
                          );
                      })}
                      {historianGroups.length === 0 && <tr><td colSpan="4" style={{ textAlign: 'center', padding: '2rem', color: '#5a6a85' }}>No historian groups configured.</td></tr>}
                  </tbody>
              </table>
          </div>
      </div>
    </div>
  );

  const renderCommunications = () => (
    <div>
      <h2 style={{ marginBottom: '1.5rem', fontSize: '1.1rem' }}>Communication Status</h2>
      {gateways.map(gw => {
        const st = gwStatus[gw.id];
        return (
          <div key={gw.id} className="comm-panel" style={{ marginBottom: '1.5rem' }}>
            <div className="comm-panel-header" style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center' }}>
              <span style={{ display: 'flex', alignItems: 'center', gap: 8 }}><div className={`gw-dot ${st?.connected ? 'gw-dot-ok' : 'gw-dot-err'}`}></div>{gw.name} — <span className="mono">{gw.ip}:{gw.port}</span></span>
              <span className={`status-pill ${st?.connected ? 'ok' : 'fault'}`}>{st?.connected ? 'TCP Connected' : 'Offline'}</span>
            </div>
            <div className="comm-stat-list">
              <div className="comm-stat"><span className="comm-key">Last Poll</span><span className="comm-val mono">{st?.lastPoll ? new Date(st.lastPoll).toLocaleTimeString() : '—'}</span></div>
              <div className="comm-stat"><span className="comm-key">TCP Error</span><span className="comm-val" style={{ color: st?.error ? '#ff3d71' : '#00e5c0' }}>{st?.error || 'None'}</span></div>
            </div>
          </div>
        );
      })}
    </div>
  );

  const renderAlarms = () => {
    return (
      <div>
        {/* ACTIVE / HISTORY ALARMS */}
        <div className="config-header" style={{ marginBottom: '1rem' }}>
          <h2>Alarm Management System <span style={{ color: '#ff3d71', marginLeft: 8 }}>({unacknowledgedAlarms.length} Active)</span></h2>
        </div>
        <div className="gateway-block">
          <table className="node-table">
            <thead><tr><th>Time</th><th>Node</th><th>Level</th><th>Message</th><th>Action</th></tr></thead>
            <tbody>
              {alarms.map(a => {
                const isWarn = a.level === 'warning';
                const cColor = isWarn ? '#ffaa00' : '#ff3d71';
                return (
                  <tr key={a.id} style={{ opacity: a.acknowledged ? 0.5 : 1, background: !a.acknowledged ? (isWarn ? 'rgba(255,170,0,0.05)' : 'rgba(255, 61, 113, 0.05)') : 'transparent' }}>
                    <td className="mono">{new Date(a.timestamp).toLocaleString()}</td>
                    <td><div style={{ display: 'flex', alignItems: 'center', gap: 8 }}><AlertTriangle size={14} color={a.acknowledged ? '#5a6a85' : cColor}/> {a.nodeName}</div></td>
                    <td>
                      <span className={`status-pill`} style={{ background: a.acknowledged ? 'rgba(255,255,255,0.05)' : (isWarn ? 'rgba(255,170,0,0.1)' : 'rgba(255,61,113,0.1)'), color: a.acknowledged ? '#5a6a85' : cColor }}>
                        {a.level.toUpperCase()}
                      </span>
                    </td>
                    <td style={{ color: a.acknowledged ? '#5a6a85' : '#e8eaf0' }}>{a.message}</td>
                    <td>
                      {!a.acknowledged ? (
                        <button className="btn btn-primary btn-sm" style={{background: cColor, color: '#fff', border: 'none'}} onClick={() => ackAlarm(a.id)}><Check size={14}/> Ack</button>
                      ) : <span style={{ fontSize: '0.8rem', color: '#00e5c0' }}>✓ Cleared</span>}
                    </td>
                  </tr>
                );
              })}
              {alarms.length === 0 && <tr><td colSpan="5" style={{ textAlign: 'center', color: '#5a6a85', padding: '2rem' }}>No alarms in history.</td></tr>}
            </tbody>
          </table>
        </div>
      </div>
    );
  };

  const renderHistorian = () => {
    const isGroupSelected = histSelection && histSelection.startsWith('group-');
    const isNodeSelected = histSelection && histSelection.startsWith('node-');
    
    const selectedGroup = isGroupSelected ? historianGroups.find(g => `group-${g.id}` === histSelection) : null;
    const groupLines = selectedGroup ? (selectedGroup.variables || []) : [];
    
    return (
      <div style={{ display: 'flex', flexDirection: 'column', flex: 1, minHeight: 0 }}>
        {/* Single compact toolbar */}
        <div style={{ display: 'flex', gap: '0.8rem', alignItems: 'center', marginBottom: '1rem', background: 'rgba(0,0,0,0.3)', padding: '0.7rem 1rem', borderRadius: 8, border: '1px solid rgba(255,255,255,0.05)', flexWrap: 'wrap' }}>
            <label style={{color: '#8896b0', fontWeight: 600, fontSize: '0.8rem', letterSpacing: '1px'}}>SELECT:</label>
            <select className="form-select" style={{ width: '220px', padding: '0.4rem 0.6rem' }} value={histSelection} onChange={e => handleHistSelectionChange(e.target.value)}>
                <option value="">-- Select Node or Group --</option>
                <optgroup label="Groups">
                    {historianGroups.map(g => (
                        <option key={`group-${g.id}`} value={`group-${g.id}`}>📁 Group: {g.name}</option>
                    ))}
                </optgroup>
                <optgroup label="Nodes">
                    {allNodes.map(n => (
                        <option key={`node-${n.id}`} value={`node-${n.id}`}>📡 {n.gwName} - {n.name}</option>
                    ))}
                </optgroup>
            </select>
            
            <label style={{color: '#8896b0', fontWeight: 600, fontSize: '0.8rem', letterSpacing: '1px'}}>FROM:</label>
            <input type="datetime-local" className="form-input" style={{ width: '170px', padding: '0.35rem 0.6rem', fontSize: '0.8rem' }} value={histFrom} onChange={e => setHistFrom(e.target.value)} />
            
            <label style={{color: '#8896b0', fontWeight: 600, fontSize: '0.8rem', letterSpacing: '1px'}}>TO:</label>
            <input type="datetime-local" className="form-input" style={{ width: '170px', padding: '0.35rem 0.6rem', fontSize: '0.8rem' }} value={histTo} onChange={e => setHistTo(e.target.value)} />
            
            <button className="btn btn-primary btn-sm" onClick={queryHistorian}>Filter</button>
            <button className="btn btn-ghost btn-sm" onClick={() => { setHistFrom(''); setHistTo(''); setHistSelection(''); setDbData([]); }}>Clear</button>
            <button className="btn btn-ghost btn-sm" onClick={queryHistorian}>↻</button>
 
            {isNodeSelected && (
              <>
                <div style={{ width: '1px', height: '24px', background: 'rgba(255,255,255,0.1)' }}></div>
                <span style={{ fontSize: '0.75rem', color: '#8896b0', textTransform: 'uppercase', letterSpacing: '1px' }}>TAGS:</span>
                {['vibX', 'vibY', 'vibZ', 'rssi', 'status'].map(tag => (
                    <label key={tag} className={`tag-checkbox-item ${histSelectedTags.includes(tag) ? 'checked' : ''}`} style={{ padding: '0.25rem 0.6rem', fontSize: '0.7rem', margin: 0 }}>
                        <input type="checkbox" checked={histSelectedTags.includes(tag)} style={{display: 'none'}} onChange={(e) => {
                            if (e.target.checked) setHistSelectedTags([...histSelectedTags, tag]);
                            else setHistSelectedTags(histSelectedTags.filter(t => t !== tag));
                        }} />
                        <div className={`gw-dot ${histSelectedTags.includes(tag) ? 'gw-dot-ok' : ''}`} style={{ background: histSelectedTags.includes(tag) ? '' : '#444', boxShadow: 'none', width: 6, height: 6 }}></div>
                        {tag.toUpperCase()}
                    </label>
                ))}
              </>
            )}

            {isGroupSelected && groupLines.length > 0 && (
              <>
                <div style={{ width: '1px', height: '24px', background: 'rgba(255,255,255,0.1)' }}></div>
                <span style={{ fontSize: '0.75rem', color: '#8896b0', textTransform: 'uppercase', letterSpacing: '1px' }}>VARIABLES:</span>
                {groupLines.map(v => {
                    const label = `${v.nodeName} - ${v.tag.toUpperCase()}`;
                    const isChecked = groupSelectionTags.includes(label);
                    return (
                        <label key={label} className={`tag-checkbox-item ${isChecked ? 'checked' : ''}`} style={{ padding: '0.25rem 0.6rem', fontSize: '0.7rem', margin: 0 }}>
                            <input type="checkbox" checked={isChecked} style={{display: 'none'}} onChange={(e) => {
                                if (e.target.checked) setGroupSelectionTags([...groupSelectionTags, label]);
                                else setGroupSelectionTags(groupSelectionTags.filter(l => l !== label));
                            }} />
                            <div className={`gw-dot ${isChecked ? 'gw-dot-ok' : ''}`} style={{ background: isChecked ? '' : '#444', boxShadow: 'none', width: 6, height: 6 }}></div>
                            {label}
                        </label>
                    );
                })}
              </>
            )}
 
            <div style={{ flex: 1 }}></div>
            <button className="btn btn-primary btn-sm" onClick={downloadCSV}><Download size={13}/> CSV</button>
        </div>
 
        {/* Chart fills ALL remaining space */}
        <div className="chart-panel" style={{ flex: 1, display: 'flex', flexDirection: 'column', minHeight: 0 }}>
            {histSelection && dbData.length > 0 ? (
                <div style={{ flex: 1, minHeight: 0 }}>
                  <ResponsiveContainer width="100%" height="100%">
                    <LineChart data={dbData} margin={{ top: 15, right: 20, left: -20, bottom: 5 }}>
                      <CartesianGrid strokeDasharray="3 3" stroke="rgba(255,255,255,0.06)" />
                      <XAxis dataKey="t" stroke="#5a6a85" fontSize={11} />
                      <YAxis stroke="#5a6a85" fontSize={11} />
                      <Tooltip contentStyle={{ background: '#0b1120', border: '1px solid rgba(255,255,255,0.1)', borderRadius: '8px' }} />
                      
                      {/* Render single node tags */}
                      {isNodeSelected && histSelectedTags.includes('vibX') && <Line type="monotone" dataKey="vibX" stroke="#ff0055" strokeWidth={2} dot={false} isAnimationActive={false} />}
                      {isNodeSelected && histSelectedTags.includes('vibY') && <Line type="monotone" dataKey="vibY" stroke="#00f0ff" strokeWidth={2} dot={false} isAnimationActive={false} />}
                      {isNodeSelected && histSelectedTags.includes('vibZ') && <Line type="monotone" dataKey="vibZ" stroke="#d000ff" strokeWidth={2} dot={false} isAnimationActive={false} />}
                      {isNodeSelected && histSelectedTags.includes('rssi') && <Line type="monotone" dataKey="rssi" stroke="#888" strokeWidth={2} dot={false} isAnimationActive={false} />}
                      {isNodeSelected && histSelectedTags.includes('status') && <Line type="stepAfter" dataKey="status" stroke="#ffaa00" strokeWidth={2} dot={false} isAnimationActive={false} />}
                      
                      {/* Render group dynamic tags */}
                      {isGroupSelected && groupLines.map((v, i) => {
                          const label = `${v.nodeName} - ${v.tag.toUpperCase()}`;
                          if (!groupSelectionTags.includes(label)) return null;
                          const colors = ['#ff0055', '#00f0ff', '#d000ff', '#ffaa00', '#00e5c0', '#ffffff', '#ff00ff', '#00ffff'];
                          const color = colors[i % colors.length];
                          return (
                              <Line key={label} type="monotone" dataKey={label} stroke={color} strokeWidth={2} dot={false} isAnimationActive={false} />
                          );
                      })}
                      
                      <Brush dataKey="t" height={30} stroke="var(--primary)" fill="#060a14" />
                    </LineChart>
                  </ResponsiveContainer>
                </div>
            ) : (
                <div className="empty-chart" style={{ flex: 1, display: 'flex', alignItems: 'center', justifyContent: 'center' }}>
                    {histSelection ? "No data found for this selection in the selected date range" : "Select a group or node to view history"}
                </div>
            )}
        </div>
      </div>
    );
  };

  const isAlarmActive = (alarm) => {
    // If it's a communication failure alarm
    if (alarm.type.startsWith('COMM_FAIL_')) {
      const gwNodeKey = Object.keys(sensorData).find(k => k.endsWith(`-${alarm.nodeId}`));
      if (!gwNodeKey) return true;
      const live = sensorData[gwNodeKey];
      return live ? !live.connected : true;
    }
    
    // If it's a rule-based alarm
    if (alarm.type.startsWith('RULE_')) {
      const ruleId = alarm.type.replace('RULE_', '');
      const rule = alarmRules.find(r => r.id === ruleId);
      if (!rule) return false;
      
      const gwNodeKey = Object.keys(sensorData).find(k => k.endsWith(`-${alarm.nodeId}`));
      if (!gwNodeKey) return true;
      const live = sensorData[gwNodeKey];
      if (!live || !live.connected) return true;
      
      const val = Number(live[rule.tag]);
      const targetVal = Number(rule.value);
      let active = false;
      if (rule.condition === '>' && val > targetVal) active = true;
      if (rule.condition === '<' && val < targetVal) active = true;
      if (rule.condition === '==' && val === targetVal) active = true;
      if (rule.condition === '>=' && val >= targetVal) active = true;
      if (rule.condition === '<=' && val <= targetVal) active = true;
      if (rule.condition === '!=' && val !== targetVal) active = true;
      
      return active;
    }
    
    return false;
  };

  const connectedCount = Object.values(gwStatus).filter(s => s.connected).length;
  const totalNodesCount = gateways.reduce((s, g) => s + g.nodes.length, 0);
  const faultNodesCount = Object.values(sensorData).filter(d => d.connected && d.status !== 1 && d.status !== 0).length;

  return (
    <div className="app-shell">
      <aside className="sidebar">
        <div style={{ display: 'flex', justifyContent: 'center', margin: '1rem 0 2rem' }}>
          <div style={{ width: '40px', height: '40px', background: 'var(--primary)', borderRadius: '8px', boxShadow: 'var(--primary-glow)', display: 'flex', alignItems: 'center', justifyContent: 'center', color: '#000' }}>
            <Activity size={24} strokeWidth={2.5} />
          </div>
        </div>
        <nav className="sidebar-nav">
          {[
            { id: 'dashboard', icon: <LayoutDashboard size={20}/>, label: 'Overview' },
            { id: 'historian', icon: <History size={20}/>, label: 'Historian' },
            { id: 'alarms', icon: <AlertTriangle size={20}/>, label: 'Alarm Manager', badge: unacknowledgedAlarms.length > 0 ? unacknowledgedAlarms.length : null },
            { id: 'config', icon: <Settings size={20}/>, label: 'Configuration' },
            { id: 'comm', icon: <Radio size={20}/>, label: 'Communications' },
          ].map(item => (
            <div key={item.id} className={`nav-item ${tab === item.id ? 'active' : ''}`} onClick={() => { setTab(item.id); setSelectedNode(null); }}>
              {item.icon}
              <span className="nav-tooltip">{item.label}</span>
              {item.badge && <span className="nav-badge-dot"></span>}
            </div>
          ))}
        </nav>
      </aside>

      <main className="main-area">
        <header className="topbar">
          <div className="topbar-title" style={{ display: 'flex', alignItems: 'center', gap: '1rem' }}>
            <span style={{ color: 'var(--primary)', fontWeight: 700, letterSpacing: '2px', textShadow: 'var(--primary-glow)' }}>VIBRATION SCADA</span>
            <span style={{ color: 'var(--border)' }}>|</span>
            <span style={{ color: 'var(--text-dim)' }}>
              {tab === 'dashboard' && (selectedNode ? `${selectedNode.node.name} — Live View` : 'System Overview')}
              {tab === 'historian' && 'Historian'}
              {tab === 'alarms' && 'Alarm Management'}
              {tab === 'config' && 'Network Configuration'}
              {tab === 'comm' && 'Communications'}
            </span>
          </div>
          <div className="topbar-badges" style={{ display: 'flex', gap: '0.8rem' }}>
            <div className={`badge ${connectedCount > 0 ? 'online' : 'offline'}`}><div className={`dot ${connectedCount > 0 ? 'pulse' : ''}`}></div> {connectedCount}/{gateways.length} Gateways</div>
            <div className="badge" style={{ background: 'rgba(255,170,0,0.1)', color: '#ffaa00', border: '1px solid rgba(255,170,0,0.3)' }}>{totalNodesCount} Nodes</div>
            <div className={`badge ${faultNodesCount > 0 ? 'offline' : 'online'}`} style={{ border: `1px solid ${faultNodesCount > 0 ? '#ff3d71' : 'rgba(0, 229, 192, 0.3)'}` }}>
                {faultNodesCount > 0 && <div className="dot pulse" style={{ background: '#ff3d71' }}></div>}
                {faultNodesCount} Faults
            </div>
          </div>
        </header>

        <div className="page-content">
          {tab === 'dashboard' && renderDashboard()}
          {tab === 'historian' && renderHistorian()}
          {tab === 'alarms' && renderAlarms()}
          {tab === 'config' && renderConfiguration()}
          {tab === 'comm' && renderCommunications()}
        </div>

        {/* GLOBAL STICKY BOTTOM ALARMS BANNER */}
        {(() => {
          const hasCritical = unacknowledgedAlarms.some(a => a.level === 'critical');
          const activeCritical = unacknowledgedAlarms.some(a => a.level === 'critical' && isAlarmActive(a));
          const activeWarning = unacknowledgedAlarms.some(a => a.level === 'warning' && isAlarmActive(a));
          
          let bannerClass = "bottom-alarm-panel";
          let headerText;
          let headerColorClass;
          let headerIcon;

          if (unacknowledgedAlarms.length === 0) {
            bannerClass += " banner-ok";
            headerText = "SYSTEM STATUS: HEALTHY";
            headerColorClass = "text-ok";
            headerIcon = <CheckCircle size={18} />;
          } else if (hasCritical) {
            bannerClass += " banner-critical";
            headerText = `SYSTEM STATUS: CRITICAL ERROR (${unacknowledgedAlarms.length})`;
            headerColorClass = `text-critical${activeCritical ? " flashing-red" : ""}`;
            headerIcon = <Bell size={18} className={activeCritical ? "pulse" : ""} />;
          } else {
            bannerClass += " banner-warning";
            headerText = `SYSTEM STATUS: WARNING ACTIVE (${unacknowledgedAlarms.length})`;
            headerColorClass = `text-warning${activeWarning ? " flashing-yellow" : ""}`;
            headerIcon = <AlertTriangle size={18} className={activeWarning ? "pulse" : ""} />;
          }

          return (
            <div className={bannerClass}>
              <div className={`bottom-alarm-header ${headerColorClass}`}>
                {headerIcon}
                <span>{headerText}</span>
              </div>
              <div className="bottom-alarm-list">
                {unacknowledgedAlarms.length > 0 ? (
                  unacknowledgedAlarms.map(a => {
                    const isWarn = a.level === 'warning';
                    const active = isAlarmActive(a);
                    return (
                      <div key={a.id} className="bottom-alarm-item">
                        <span style={{ fontWeight: 'bold', color: '#fff' }}>{a.nodeName}</span>
                        <span style={{ color: isWarn ? '#ffaa00' : '#ff3d71' }}>[{a.level.toUpperCase()}]</span>
                        <span style={{ color: '#e8eaf0' }}>{a.message}</span>
                        {active ? (
                          <span style={{ color: '#ff3d71', fontSize: '0.75rem', fontWeight: 'bold', marginLeft: '0.5rem', display: 'inline-flex', alignItems: 'center', gap: '4px' }}>
                            <span className="dot pulse" style={{ width: 6, height: 6, background: '#ff3d71', boxShadow: '0 0 6px #ff3d71' }}></span> Active
                          </span>
                        ) : (
                          <button className="btn btn-ghost btn-sm ack-btn" style={{ border: 'none' }} onClick={() => ackAlarm(a.id)}>✓ Ack</button>
                        )}
                      </div>
                    );
                  })
                ) : (
                  <div className="bottom-alarm-nominal-msg">
                    <Check size={14} /> All systems operational. No unacknowledged alarms.
                  </div>
                )}
              </div>
            </div>
          );
        })()}
      </main>

      {/* MODALS */}
      {showAddGw && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header"><div className="modal-title">{editGw ? 'Edit Gateway' : 'Add Modbus Gateway'}</div><button className="modal-close" onClick={() => setShowAddGw(false)}>×</button></div>
            <div className="form-group"><label className="form-label">Gateway Name</label><input className="form-input" value={gwForm.name} onChange={e => setGwForm({ ...gwForm, name: e.target.value })} placeholder="e.g. Factory Floor 1" /></div>
            <div className="form-row">
              <div className="form-group"><label className="form-label">IP Address</label><input className="form-input" value={gwForm.ip} onChange={e => setGwForm({ ...gwForm, ip: e.target.value })} placeholder="192.168.1.100" /></div>
              <div className="form-group"><label className="form-label">TCP Port</label><input type="number" className="form-input" value={gwForm.port} onChange={e => setGwForm({ ...gwForm, port: parseInt(e.target.value) })} /></div>
            </div>
            <div className="modal-footer"><button className="btn btn-ghost" onClick={() => setShowAddGw(false)}>Cancel</button><button className="btn btn-primary" onClick={saveGateway}><Save size={14}/> Save</button></div>
          </div>
        </div>
      )}

      {showAddNode && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header"><div className="modal-title">{editNode ? 'Edit Node' : 'Add Sensor Node'}</div><button className="modal-close" onClick={() => setShowAddNode(false)}>×</button></div>
            <div className="form-group"><label className="form-label">Node Name</label><input className="form-input" value={nodeForm.name} onChange={e => setNodeForm({ ...nodeForm, name: e.target.value })} placeholder="e.g. Pump 02" /></div>

            <div className="form-row">
              <div className="form-group"><label className="form-label">Modbus Unit ID (Slave ID)</label><input type="number" min="1" max="247" className="form-input" value={nodeForm.unitId || 1} onChange={e => setNodeForm({ ...nodeForm, unitId: parseInt(e.target.value) || 1 })} /></div>
              <div className="form-group"><label className="form-label">Status Reg</label><input type="number" className="form-input" value={nodeForm.statusReg} onChange={e => setNodeForm({ ...nodeForm, statusReg: parseInt(e.target.value) })} /></div>
            </div>
            <div className="form-row">
              <div className="form-group"><label className="form-label">Vib X Reg</label><input type="number" className="form-input" value={nodeForm.vibXReg} onChange={e => setNodeForm({ ...nodeForm, vibXReg: parseInt(e.target.value) })} /></div>
              <div className="form-group"><label className="form-label">Vib Y Reg</label><input type="number" className="form-input" value={nodeForm.vibYReg} onChange={e => setNodeForm({ ...nodeForm, vibYReg: parseInt(e.target.value) })} /></div>
            </div>
            <div className="form-row">
              <div className="form-group"><label className="form-label">Vib Z Reg</label><input type="number" className="form-input" value={nodeForm.vibZReg} onChange={e => setNodeForm({ ...nodeForm, vibZReg: parseInt(e.target.value) })} /></div>
              <div className="form-group"><label className="form-label">RSSI Reg</label><input type="number" className="form-input" value={nodeForm.rssiReg} onChange={e => setNodeForm({ ...nodeForm, rssiReg: parseInt(e.target.value) })} /></div>
            </div>
            <div className="modal-footer"><button className="btn btn-ghost" onClick={() => setShowAddNode(false)}>Cancel</button><button className="btn btn-primary" onClick={saveNode}><Save size={14}/> Save Node</button></div>
          </div>
        </div>
      )}

      {/* MODAL: ADD/EDIT ALARM RULE */}
      {showAddRule && (
        <div className="modal-overlay">
          <div className="modal">
            <div className="modal-header">
              <div className="modal-title">{editRule ? 'Edit Alarm Rule' : 'Configure New Alarm Rule'}</div>
              <button className="modal-close" onClick={() => { setShowAddRule(false); setEditRule(null); }}>×</button>
            </div>
            
            <div className="form-group">
                <label className="form-label">Target Node</label>
                <select className="form-select" value={ruleForm.nodeId} onChange={e => setRuleForm({...ruleForm, nodeId: e.target.value})}>
                    <option value="ANY">Any Node (All Sensors)</option>
                    {allNodes.map(n => <option key={n.id} value={n.id}>{n.gwName} - {n.name}</option>)}
                </select>
            </div>
            
            <div className="form-row">
                <div className="form-group">
                    <label className="form-label">Tag (Variable)</label>
                    <select className="form-select" value={ruleForm.tag} onChange={e => setRuleForm({...ruleForm, tag: e.target.value})}>
                        <option value="vibX">Vibration X</option>
                        <option value="vibY">Vibration Y</option>
                        <option value="vibZ">Vibration Z</option>
                        <option value="rssi">Signal Strength (RSSI)</option>
                        <option value="status">Status Register</option>
                    </select>
                </div>
                <div className="form-group">
                    <label className="form-label">Alarm Level</label>
                    <select className="form-select" value={ruleForm.level} onChange={e => setRuleForm({...ruleForm, level: e.target.value})}>
                        <option value="warning">Warning (Yellow)</option>
                        <option value="critical">Critical (Red)</option>
                    </select>
                </div>
            </div>

            <div className="form-row">
                <div className="form-group">
                    <label className="form-label">Condition</label>
                    <select className="form-select" value={ruleForm.condition} onChange={e => setRuleForm({...ruleForm, condition: e.target.value})}>
                        <option value=">">Greater Than (&gt;)</option>
                        <option value="<">Less Than (&lt;)</option>
                        <option value=">=">Greater or Equal (&gt;=)</option>
                        <option value="<=">Less or Equal (&lt;=)</option>
                        <option value="==">Exactly Equal (==)</option>
                        <option value="!=">Not Equal (!=)</option>
                    </select>
                </div>
                <div className="form-group">
                    <label className="form-label">Value</label>
                    <input type="number" step="0.1" className="form-input" value={ruleForm.value} onChange={e => setRuleForm({...ruleForm, value: parseFloat(e.target.value)})} />
                </div>
            </div>

            <div className="form-group">
                <label className="form-label">Custom Alarm Message</label>
                <input className="form-input" value={ruleForm.message} onChange={e => setRuleForm({...ruleForm, message: e.target.value})} placeholder="e.g. Danger: Vibration X exceeded limit!" />
            </div>

            <div className="modal-footer"><button className="btn btn-ghost" onClick={() => { setShowAddRule(false); setEditRule(null); }}>Cancel</button><button className="btn btn-primary" onClick={saveRule}><Save size={14}/> Save Rule</button></div>
          </div>
        </div>
      )}

      {/* MODAL: ADD/EDIT HISTORIAN GROUP */}
      {showAddGroup && (
        <div className="modal-overlay">
          <div className="modal" style={{ maxWidth: '600px' }}>
            <div className="modal-header">
              <div className="modal-title">{editGroup ? 'Edit Historian Group' : 'Create Historian Group'}</div>
              <button className="modal-close" onClick={() => { setShowAddGroup(false); setEditGroup(null); }}>×</button>
            </div>
            
            <div className="form-group">
              <label className="form-label">Group Name</label>
              <input 
                className="form-input" 
                value={groupForm.name} 
                onChange={e => setGroupForm({ ...groupForm, name: e.target.value })} 
                placeholder="e.g. d1" 
              />
            </div>
            
            <div style={{ marginBottom: '1.5rem' }}>
              <div style={{ display: 'flex', justifyContent: 'space-between', alignItems: 'center', marginBottom: '0.8rem' }}>
                <span className="form-label" style={{ margin: 0 }}>Group Variables</span>
                <button 
                  className="btn btn-ghost btn-sm" 
                  onClick={() => {
                    const defaultVar = allNodes[0] ? { nodeId: allNodes[0].id, nodeName: allNodes[0].name, tag: 'vibX' } : { nodeId: '', nodeName: '', tag: 'vibX' };
                    setGroupForm({ ...groupForm, variables: [...groupForm.variables, defaultVar] });
                  }}
                  style={{ border: '1px solid var(--border)' }}
                >
                  ➕ Add Variable
                </button>
              </div>
              
              <div style={{ display: 'flex', flexDirection: 'column', gap: '0.6rem', maxHeight: '250px', overflowY: 'auto', paddingRight: '4px' }}>
                {groupForm.variables.map((v, index) => (
                  <div key={index} style={{ display: 'flex', gap: '0.8rem', alignItems: 'center', background: 'rgba(0,0,0,0.2)', padding: '0.5rem 0.8rem', borderRadius: '6px', border: '1px solid rgba(255,255,255,0.05)' }}>
                    <div style={{ flex: 2 }}>
                      <select 
                        className="form-select" 
                        value={v.nodeId} 
                        onChange={e => {
                          const selectedNodeId = e.target.value;
                          const selectedNode = allNodes.find(n => n.id === selectedNodeId);
                          const updatedVars = [...groupForm.variables];
                          updatedVars[index] = { 
                            ...updatedVars[index], 
                            nodeId: selectedNodeId, 
                            nodeName: selectedNode ? selectedNode.name : '' 
                          };
                          setGroupForm({ ...groupForm, variables: updatedVars });
                        }}
                        style={{ padding: '0.4rem' }}
                      >
                        <option value="">-- Select Node --</option>
                        {allNodes.map(n => <option key={n.id} value={n.id}>{n.gwName} - {n.name}</option>)}
                      </select>
                    </div>
                    
                    <div style={{ flex: 1.5 }}>
                      <select 
                        className="form-select" 
                        value={v.tag} 
                        onChange={e => {
                          const updatedVars = [...groupForm.variables];
                          updatedVars[index] = { ...updatedVars[index], tag: e.target.value };
                          setGroupForm({ ...groupForm, variables: updatedVars });
                        }}
                        style={{ padding: '0.4rem' }}
                      >
                        <option value="vibX">Vib X</option>
                        <option value="vibY">Vib Y</option>
                        <option value="vibZ">Vib Z</option>
                        <option value="rssi">RSSI</option>
                        <option value="status">Status</option>
                      </select>
                    </div>
                    
                    <button 
                      className="btn btn-danger btn-sm" 
                      onClick={() => {
                        const updatedVars = groupForm.variables.filter((_, idx) => idx !== index);
                        setGroupForm({ ...groupForm, variables: updatedVars });
                      }}
                      style={{ padding: '0.4rem 0.6rem' }}
                    >
                      ✕
                    </button>
                  </div>
                ))}
                {groupForm.variables.length === 0 && (
                  <div style={{ textAlign: 'center', color: '#5a6a85', fontSize: '0.8rem', padding: '1.5rem', border: '1px dashed var(--border)', borderRadius: '6px' }}>
                    No variables in this group. Click "Add Variable" above.
                  </div>
                )}
              </div>
            </div>

            <div className="modal-footer">
              <button className="btn btn-ghost" onClick={() => { setShowAddGroup(false); setEditGroup(null); }}>Cancel</button>
              <button className="btn btn-primary" onClick={saveHistorianGroup}><Save size={14}/> Save Group</button>
            </div>
          </div>
        </div>
      )}

    </div>
  );
}
