import React, { useState, useEffect } from 'react';
import io from 'socket.io-client';
import { LineChart, Line, XAxis, YAxis, CartesianGrid, Tooltip, ResponsiveContainer } from 'recharts';
import { Activity, Wifi, AlertTriangle, Server } from 'lucide-react';
import './App.css';

const socket = io('http://localhost:3001');

function App() {
  const [dataHistory, setDataHistory] = useState([]);
  const [currentData, setCurrentData] = useState({ vibX: 0, vibY: 0, vibZ: 0, rssi: 0, status: 0 });
  const [gatewayStatus, setGatewayStatus] = useState({ connected: false, ip: '' });

  useEffect(() => {
    socket.on('gatewayStatus', (status) => setGatewayStatus(status));
    
    socket.on('sensorData', (data) => {
      setCurrentData(data);
      setDataHistory(prev => {
        const newHistory = [...prev, data];
        if (newHistory.length > 20) newHistory.shift(); // Keep last 20 points
        return newHistory;
      });
    });

    return () => {
      socket.off('gatewayStatus');
      socket.off('sensorData');
    };
  }, []);

  return (
    <div className="dashboard-container">
      <header className="header glass">
        <div className="logo">
          <Activity size={32} color="#00ffcc" />
          <h1>Industrial SCADA | Predictive Maintenance</h1>
        </div>
        <div className="status-indicators">
          <div className={`status-badge ${gatewayStatus.connected ? 'active' : 'offline'}`}>
            <Server size={18} /> 
            Modbus Gateway: {gatewayStatus.connected ? gatewayStatus.ip : 'DISCONNECTED'}
          </div>
          <div className={`status-badge ${currentData.status === 1 ? 'active' : 'warning'}`}>
            {currentData.status === 1 ? <Wifi size={18} /> : <AlertTriangle size={18} />}
            Edge Node: {currentData.status === 1 ? 'NORMAL' : 'FAULT'}
          </div>
        </div>
      </header>

      <main className="main-content">
        <div className="cards-grid">
          
          {/* Vibration Cards */}
          <div className="data-card glass">
            <h3>Vibration X-Axis</h3>
            <div className="value">{currentData.vibX} <span className="unit">Hz</span></div>
          </div>
          <div className="data-card glass">
            <h3>Vibration Y-Axis</h3>
            <div className="value">{currentData.vibY} <span className="unit">Hz</span></div>
          </div>
          <div className="data-card glass">
            <h3>Vibration Z-Axis</h3>
            <div className="value">{currentData.vibZ} <span className="unit">Hz</span></div>
          </div>
          <div className="data-card glass">
            <h3>Signal Strength (RSSI)</h3>
            <div className="value">{currentData.rssi} <span className="unit">dBm</span></div>
          </div>
        </div>

        {/* Live Chart */}
        <div className="chart-container glass">
          <h2>Live Vibration Analysis</h2>
          <div style={{ width: '100%', height: 350 }}>
            <ResponsiveContainer>
              <LineChart data={dataHistory} margin={{ top: 20, right: 30, left: 0, bottom: 0 }}>
                <CartesianGrid strokeDasharray="3 3" stroke="#333" />
                <XAxis dataKey="timestamp" tickFormatter={(time) => new Date(time).toLocaleTimeString()} stroke="#888" />
                <YAxis stroke="#888" />
                <Tooltip contentStyle={{ backgroundColor: '#1a1a2e', borderColor: '#00ffcc' }} />
                <Line type="monotone" dataKey="vibX" stroke="#ff007a" strokeWidth={3} dot={false} name="X-Axis" />
                <Line type="monotone" dataKey="vibY" stroke="#00ffcc" strokeWidth={3} dot={false} name="Y-Axis" />
                <Line type="monotone" dataKey="vibZ" stroke="#7a00ff" strokeWidth={3} dot={false} name="Z-Axis" />
              </LineChart>
            </ResponsiveContainer>
          </div>
        </div>
      </main>
    </div>
  );
}

export default App;
