#define HELTEC_POWER_BUTTON
#include <heltec_unofficial.h>
#include <WiFi.h>
#include <ModbusIP_ESP8266.h>

const char* ssid = "YOUR_WIFI_SSID";
const char* password = "YOUR_WIFI_PASSWORD";

ModbusIP mb; // Modbus IP Server

String rxdata;
volatile bool rxFlag = false;

void rx() { rxFlag = true; }

void setup() {
  heltec_setup();
  Serial.println("=== GATEWAY & MODBUS SERVER READY ===");

  // 1. Connect to WiFi
  Serial.print("Connecting to WiFi");
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("\nWiFi connected!");
  Serial.print("Modbus Server IP address: ");
  Serial.println(WiFi.localIP());

  // 2. Setup Modbus TCP Server
  mb.server();
  // Create Holding Registers 0 through 4
  mb.addHreg(0, 0); // Register 0: Node Status (1 = Normal)
  mb.addHreg(1, 0); // Register 1: Vibration X
  mb.addHreg(2, 0); // Register 2: Vibration Y
  mb.addHreg(3, 0); // Register 3: Vibration Z
  mb.addHreg(4, 0); // Register 4: RSSI

  // 3. Setup LoRa Radio
  RADIOLIB_OR_HALT(radio.begin());
  radio.setDio1Action(rx);
  RADIOLIB_OR_HALT(radio.setFrequency(868.0));
  RADIOLIB_OR_HALT(radio.setBandwidth(125.0));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(7));
  RADIOLIB_OR_HALT(radio.setCodingRate(5));
  RADIOLIB_OR_HALT(radio.setPreambleLength(8));
  RADIOLIB_OR_HALT(radio.setSyncWord(RADIOLIB_SX126X_SYNC_WORD_PRIVATE));

  Serial.println("Listening for sensor nodes...");
  RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
}

void loop() {
  heltec_loop();
  
  // Handle Modbus requests from PLC or Web Backend
  mb.task();

  if (rxFlag) {
    rxFlag = false;
    int state = radio.readData(rxdata);
    if (state == RADIOLIB_ERR_NONE) {
      Serial.println("====== PACKET RECEIVED ======");
      Serial.print("Data: ");   Serial.println(rxdata);
      
      float rssi = radio.getRSSI();
      Serial.print("RSSI: ");   Serial.print(rssi); Serial.println(" dBm");
      
      // Parse data and update Modbus registers
      // Assuming payload is "M1: NORMAL"
      if (rxdata.indexOf("NORMAL") != -1) {
        mb.Hreg(0, 1); // 1 = Normal status
      } else {
        mb.Hreg(0, 0); // 0 = Fault/Error
      }
      
      // Update dummy vibration data for now (You can parse real data later)
      mb.Hreg(1, random(10, 50)); // Dummy Vib X
      mb.Hreg(2, random(10, 50)); // Dummy Vib Y
      mb.Hreg(3, random(10, 50)); // Dummy Vib Z
      
      // Store absolute RSSI value
      mb.Hreg(4, abs((int)rssi));

      Serial.println("Modbus Registers Updated.");
      Serial.println("=============================");
    }
    RADIOLIB_OR_HALT(radio.startReceive(RADIOLIB_SX126X_RX_TIMEOUT_INF));
  }
}