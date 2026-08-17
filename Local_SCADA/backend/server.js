const express = require('express');
const http = require('http');
const { Server } = require('socket.io');
const ModbusRTU = require('modbus-serial');
const cors = require('cors');

const app = express();
app.use(cors());

const server = http.createServer(app);
const io = new Server(server, {
    cors: {
        origin: "*",
        methods: ["GET", "POST"]
    }
});

// Modbus TCP Client
const client = new ModbusRTU();
const ESP32_IP = "192.168.1.100"; // REPLACE with the actual IP of your ESP32
const ESP32_PORT = 502;

let isConnected = false;

// Connect to ESP32 Gateway
async function connectModbus() {
    try {
        console.log(`Attempting to connect to Modbus TCP at ${ESP32_IP}:${ESP32_PORT}...`);
        await client.connectTCP(ESP32_IP, { port: ESP32_PORT });
        client.setID(1);
        isConnected = true;
        console.log("Connected to Modbus Gateway!");
    } catch (e) {
        console.error("Modbus Connection Error:", e.message);
        isConnected = false;
        setTimeout(connectModbus, 5000); // Retry in 5s
    }
}

connectModbus();

// Polling Loop (Read data every 1 second)
setInterval(async () => {
    if (isConnected) {
        try {
            // Read 5 holding registers starting at address 0
            // Reg 0: Node Status (1 = Normal, 0 = Error)
            // Reg 1: Vibration X
            // Reg 2: Vibration Y
            // Reg 3: Vibration Z
            // Reg 4: Signal Strength (RSSI)
            const data = await client.readHoldingRegisters(0, 5);
            
            const sensorData = {
                timestamp: new Date().toISOString(),
                status: data.data[0],
                vibX: data.data[1],
                vibY: data.data[2],
                vibZ: data.data[3],
                rssi: data.data[4] * -1 // Convert back to negative dBm
            };

            // Send to React Frontend
            io.emit('sensorData', sensorData);
            
        } catch (e) {
            console.error("Failed to read Modbus:", e.message);
            isConnected = false;
            client.close();
            setTimeout(connectModbus, 5000);
        }
    }
}, 1000);

// Socket.io connection handling
io.on('connection', (socket) => {
    console.log('React Frontend Connected:', socket.id);
    socket.emit('gatewayStatus', { connected: isConnected, ip: ESP32_IP });
    
    socket.on('disconnect', () => {
        console.log('React Frontend Disconnected:', socket.id);
    });
});

const PORT = 3001;
server.listen(PORT, () => {
    console.log(`SCADA Backend running on http://localhost:${PORT}`);
});
