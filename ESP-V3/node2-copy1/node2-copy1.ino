#include <heltec_unofficial.h>
#include <math.h>

// --- ADXL355 SENSOR PINS (Right Rail Block: 4, 5, 6, 7) ---
const int CS_PIN   = 4;  // Chip Select
const int MOSI_PIN = 5;  // Master Out Slave In
const int MISO_PIN = 6;  // Master In Slave Out
const int SCK_PIN  = 7;  // Serial Clock

// --- NODE CONFIGURATION ---
String NODE_ID = "M2"; 
int simBattery = 92;

// ==========================================
// PURE SOFTWARE SPI DRIVER
// ==========================================
uint8_t swSPI_transfer(uint8_t value) {
  uint8_t out = 0;
  for (int i = 0; i < 8; i++) {
    digitalWrite(MOSI_PIN, (value & 0x80) ? HIGH : LOW);
    value <<= 1;
    digitalWrite(SCK_PIN, HIGH);
    delayMicroseconds(2); 
    out <<= 1;
    if (digitalRead(MISO_PIN)) out |= 0x01;
    digitalWrite(SCK_PIN, LOW);
    delayMicroseconds(2);
  }
  return out;
}

// ==========================================
// SETUP
// ==========================================
void setup() {
  heltec_setup();
  Serial.begin(115200);

  // Initialize Radio (using your tested parameters)
  RADIOLIB_OR_HALT(radio.begin());
  RADIOLIB_OR_HALT(radio.setFrequency(868.0));
  RADIOLIB_OR_HALT(radio.setSpreadingFactor(7));
  RADIOLIB_OR_HALT(radio.setSyncWord(RADIOLIB_SX126X_SYNC_WORD_PRIVATE));
  radio.setOutputPower(10);

  // Initialize Software SPI Pins
  pinMode(CS_PIN, OUTPUT);
  pinMode(SCK_PIN, OUTPUT);
  pinMode(MOSI_PIN, OUTPUT);
  pinMode(MISO_PIN, INPUT);
  digitalWrite(CS_PIN, HIGH);
  digitalWrite(SCK_PIN, LOW);

  // Wake sensor
  digitalWrite(CS_PIN, LOW);
  swSPI_transfer((0x2D << 1) | 0x00); 
  swSPI_transfer(0x00);               
  digitalWrite(CS_PIN, HIGH);

  Serial.println("\n--- Node M2 Fully Armed ---");
}

// ==========================================
// LOOP
// ==========================================
void loop() {
  heltec_loop();

  // 1. Read Sensor
  uint8_t buffer[9];
  digitalWrite(CS_PIN, LOW);
  swSPI_transfer((0x08 << 1) | 0x01); 
  for (int i = 0; i < 9; i++) buffer[i] = swSPI_transfer(0x00);
  digitalWrite(CS_PIN, HIGH);

  uint32_t rawX = ((uint32_t)buffer[0] << 12) | ((uint32_t)buffer[1] << 4) | (buffer[2] >> 4);
  uint32_t rawY = ((uint32_t)buffer[3] << 12) | ((uint32_t)buffer[4] << 4) | (buffer[5] >> 4);
  uint32_t rawZ = ((uint32_t)buffer[6] << 12) | ((uint32_t)buffer[7] << 4) | (buffer[8] >> 4);

  if (rawX & 0x80000) rawX |= 0xFFF00000;
  if (rawY & 0x80000) rawY |= 0xFFF00000;
  if (rawZ & 0x80000) rawZ |= 0xFFF00000;

  float accX = (int32_t)rawX * 0.0000039;
  float accY = (int32_t)rawY * 0.0000039;
  float accZ = (int32_t)rawZ * 0.0000039;
  float totalMagnitude = sqrt((accX * accX) + (accY * accY) + (accZ * accZ));

  // 2. Build Payload
  String machineStatus = (abs(totalMagnitude - 1.0) > 0.20) ? "ALERTE" : "NORMAL";
  String payload = "{\"id\":\"" + NODE_ID + "\",\"st\":\"" + machineStatus + "\",\"v\":" + String(totalMagnitude, 2) + ",\"b\":" + String(simBattery) + "}";

  // 3. Transmit
  Serial.println("TX: " + payload);
  int result = radio.transmit(payload);
  
  if (result == RADIOLIB_ERR_NONE) Serial.println("   OK!");
  else Serial.println("   FAILED!");

  heltec_delay(5000); 
}