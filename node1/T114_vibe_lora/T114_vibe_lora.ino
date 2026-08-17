/*
 * T114_vibe_lora.ino -- on-board AI vibration diagnosis + LoRa transmission
 * =====================================================================
 * Heltec Mesh Node T114 (nRF52840 + SX1262)
 *
 * Combines:
 *  - the AI pipeline from T114_vibe_bridge.ino (vibe_ai.h / model_data.h):
 *    ADXL355 -> 21 features -> random forest -> diagnosis, once per
 *    1024-sample window (~1.024 s @ 1000 Hz)
 *  - the LoRa radio + display init from node-copy2.ino, which uses this
 *    board's own driver (Radio.* via heltec_nrf_lorawan.h / boardInit()),
 *    NOT RadioLib / heltec_unofficial.h (those are for the ESP32 Heltec
 *    boards and don't apply to the T114's nRF52840 + SX1262).
 *
 * Every AI window the node transmits:
 *   {"id":"M1","st":<1-5>,"conf":0.xx,"vx":..,"vy":..,"vz":..,"b":<batt>}
 *
 * "st" is translated from the model's CLASS_NAMES into the numeric codes
 * the gateway PLC expects (see Network 4 of the PLC logic):
 *   1 = healthy      2 = imbalance    3 = misalignment
 *   4 = looseness    5 = bearing_fault
 * 0/OFFLINE is inferred by the PLC from missing data (status word stays 0
 * and the "signal" flag stays 0), so the node itself never sends 0.
 *
 * SETUP: put model_data.h and vibe_ai.h (same folder they came from) next
 * to this .ino before compiling.
 * ADXL355 wiring (same as T114_vibe_bridge.ino):
 *   VCC->3V3 GND->GND SCLK->47 CS->44 MOSI->10 MISO->33
 *
 * Serial Monitor (115200) data-source switch:
 *   MODE:SENSOR       -> read the real ADXL355 (default at boot)
 *   MODE:SIM          -> ignore the sensor, feed it fake samples instead
 *   D:<x>,<y>,<z>      -> one simulated sample in milli-g (SIM mode only)
 *   RPM:<value>        -> set the shaft speed used by the feature extractor
 * =====================================================================
 */

#include "Arduino.h"
#include "heltec_nrf_lorawan.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "vibe_ai.h"          // feature recipe + forest (includes model_data.h)

// ---------------- node identity ----------------
#define NODE_ID "M1"

// ---------------- Display Pins (SPI1) ----------------
#define TFT_CS        11
#define TFT_DC        12
#define TFT_RST       2
#define TFT_SCK       40
#define TFT_MOSI      41
#define TFT_MISO      43
#define TFT_VDD_EN    3
#define TFT_LEDA_EN   15

// ---------------- LoRa Settings — SF9 + 250 kHz = max range at 1s update rate ----------------
#define RF_FREQUENCY                868000000  // 868 MHz for Europe
#define TX_OUTPUT_POWER             22         // dBm — maximum power for SX1262
#define LORA_BANDWIDTH              1          // 250 kHz (wider = faster air time)
#define LORA_SPREADING_FACTOR       9          // SF9 = better range than SF7
#define LORA_CODINGRATE             1          // 4/5
#define LORA_PREAMBLE_LENGTH        8
#define LORA_FIX_LENGTH_PAYLOAD_ON  false
#define LORA_IQ_INVERSION_ON        false
#define BUFFER_SIZE                 96

// Display
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI1, TFT_CS, TFT_DC, TFT_RST);

// Radio
char txpacket[BUFFER_SIZE];
static RadioEvents_t RadioEvents;
volatile bool radioBusy = false;

void OnTxDone(void);
void OnTxTimeout(void);

// ---------------- ADXL355 (software SPI) ----------------
const int PIN_CS = 44, PIN_SCK = 47, PIN_MOSI = 10, PIN_MISO = 33;
const uint8_t REG_DEVID_AD = 0x00, REG_XDATA3 = 0x08;
const uint8_t REG_FILTER = 0x28, REG_RANGE = 0x2C, REG_POWER_CTL = 0x2D, REG_RESET = 0x2F;
const uint8_t RANGE_2G = 0x01, ODR_1000HZ = 0x02;
const float   LSB_PER_G = 256000.0f;

const uint32_t SAMPLE_HZ = 1000;
const uint32_t SAMPLE_US = 1000000UL / SAMPLE_HZ;

// data source switch, set over Serial with "MODE:SIM" / "MODE:SENSOR"
enum { SENSOR_MODE = 0, SIM_MODE = 1 };
int mode = SENSOR_MODE;

bool haveSensor = false;
float gx = 0, gy = 0, gz = 0;
float rpmSetting = 1800.0f;      // shaft speed used for the FFT band windows; adjust to your machine
uint32_t sampleCount = 0, lastRate = 0, rateStart = 0, lastTick = 0, lastRx = 0;
char line[48]; int lineLen = 0;

// AI window buffers (1024 samples per axis = ~1.024 s)
static float xw[AI_WIN], yw[AI_WIN], zw[AI_WIN];
int wfill = 0;
// diagnosis smoothing: 3 identical windows in a row = "confirmed" (used for
// the Serial/display readout only; every window is still transmitted)
int lastPred = -1, streak = 0, confirmed = -1;

// CLASS_NAMES order: bearing_fault, healthy, imbalance, looseness, misalignment
// PLC status codes:  1=healthy 2=imbalance 3=misalignment 4=looseness 5=bearing_fault
const int STATUS_CODE[N_CLASSES] = {5, 1, 2, 4, 3};

// ---------------- software SPI ----------------
uint8_t spiByte(uint8_t out) {
  uint8_t in = 0;
  for (int i = 7; i >= 0; i--) {
    digitalWrite(PIN_MOSI, (out >> i) & 1);
    digitalWrite(PIN_SCK, HIGH);
    in = (in << 1) | (digitalRead(PIN_MISO) & 1);
    digitalWrite(PIN_SCK, LOW);
  }
  return in;
}
uint8_t readReg(uint8_t r) {
  digitalWrite(PIN_CS, LOW); spiByte((r << 1) | 1);
  uint8_t v = spiByte(0); digitalWrite(PIN_CS, HIGH); return v;
}
void writeReg(uint8_t r, uint8_t v) {
  digitalWrite(PIN_CS, LOW); spiByte((r << 1) | 0); spiByte(v); digitalWrite(PIN_CS, HIGH);
}
void readBurst(uint8_t r, uint8_t *b, uint8_t n) {
  digitalWrite(PIN_CS, LOW); spiByte((r << 1) | 1);
  for (uint8_t i = 0; i < n; i++) b[i] = spiByte(0);
  digitalWrite(PIN_CS, HIGH);
}
int32_t to20(uint8_t a, uint8_t b, uint8_t c) {
  int32_t v = ((int32_t)a << 12) | ((int32_t)b << 4) | (c >> 4);
  if (v & 0x80000) v |= 0xFFF00000; return v;
}
bool adxlInit() {
  pinMode(PIN_CS, OUTPUT); digitalWrite(PIN_CS, HIGH);
  pinMode(PIN_SCK, OUTPUT); digitalWrite(PIN_SCK, LOW);
  pinMode(PIN_MOSI, OUTPUT); pinMode(PIN_MISO, INPUT);
  if (readReg(REG_DEVID_AD) != 0xAD) return false;
  writeReg(REG_RESET, 0x52); delay(10);
  writeReg(REG_RANGE, RANGE_2G);
  writeReg(REG_FILTER, ODR_1000HZ);
  writeReg(REG_POWER_CTL, 0x00); delay(10);
  return true;
}
void readSensor() {
  uint8_t raw[9]; readBurst(REG_XDATA3, raw, 9);
  gx = to20(raw[0], raw[1], raw[2]) / LSB_PER_G;
  gy = to20(raw[3], raw[4], raw[5]) / LSB_PER_G;
  gz = to20(raw[6], raw[7], raw[8]) / LSB_PER_G;
}

// ---------------- battery ----------------
int readBatteryPercent() {
  // Placeholder so the payload format matches node-copy2.ino.
  // Wire this up to the board's battery ADC pin/divider for a real reading.
  return 85;
}

// ---------------- display ----------------
void showStatus(const char *label, float conf, int code) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 10);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.println("--- SENSOR NODE ---");
  tft.setCursor(0, 45);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("ID: "); tft.println(NODE_ID);
  tft.setCursor(0, 70);
  tft.setTextColor(ST77XX_YELLOW);
  tft.print("Dx: "); tft.println(label);
  tft.setCursor(0, 95);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Code: "); tft.print(code);
  tft.print("  p="); tft.println(conf, 2);
  tft.setCursor(0, 120);
  tft.setTextColor(ST77XX_GREEN);
  tft.println("Transmitting...");
}

// ---------------- serial protocol (data source switch) ----------------
// Send over Serial Monitor / from an app:
//   "MODE:SENSOR"   -> read the real ADXL355 (default)
//   "MODE:SIM"      -> ignore the sensor, wait for "D:" lines instead
//   "D:<x>,<y>,<z>" -> one simulated sample in milli-g (only used in SIM mode)
//   "RPM:<value>"   -> set the shaft speed used by the feature extractor
void parseLine(char *s) {
  if (!strncmp(s, "MODE:", 5)) {
    mode = strstr(s, "SIM") ? SIM_MODE : SENSOR_MODE;
    sampleCount = 0; rateStart = millis();
    wfill = 0; lastPred = -1; streak = 0; confirmed = -1;   // fresh window
    Serial.println(mode == SIM_MODE ? "MODE SET: SIMULATOR" : "MODE SET: REAL SENSOR");
  } else if (!strncmp(s, "RPM:", 4)) {
    float r = atof(s + 4);
    if (r >= 60 && r <= 12000) { rpmSetting = r; }
    Serial.print("RPM SET: "); Serial.println(rpmSetting, 0);
  } else if (s[0] == 'D' && s[1] == ':') {
    if (mode == SIM_MODE) {
      char *p = s + 2;
      long x = strtol(p, &p, 10); if (*p) p++;
      long y = strtol(p, &p, 10); if (*p) p++;
      long z = strtol(p, &p, 10);
      gx = x / 1000.0f; gy = y / 1000.0f; gz = z / 1000.0f;
      lastRx = millis(); sampleCount++;
      addSample(gx, gy, gz);
    }
  }
}
void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n') { line[lineLen] = 0; parseLine(line); lineLen = 0; }
    else if (c != '\r' && lineLen < (int)sizeof(line) - 1) line[lineLen++] = c;
  }
}

// ---------------- LoRa send ----------------
void sendPayload(int code, float conf) {
  String payload = String("{\"id\":\"") + NODE_ID + "\",\"st\":" + String(code) +
                    ",\"conf\":" + String(conf, 2) +
                    ",\"vx\":" + String(gx, 3) + ",\"vy\":" + String(gy, 3) +
                    ",\"vz\":" + String(gz, 3) + ",\"b\":" + String(readBatteryPercent()) + "}";
  payload.toCharArray(txpacket, BUFFER_SIZE);
  Serial.printf("Sending: \"%s\"\n", txpacket);
  radioBusy = true;
  Radio.Send((uint8_t *)txpacket, strlen(txpacket));
}

// ---------------- AI window handling ----------------
void addSample(float x, float y, float z) {
  xw[wfill] = x; yw[wfill] = y; zw[wfill] = z;
  if (++wfill >= AI_WIN) { wfill = 0; runAI(); }
}
void runAI() {
  float feats[N_FEATURES], probs[N_CLASSES];
  ai_extract_features(xw, yw, zw, rpmSetting, feats);
  int pred = ai_predict(feats, probs);

  Serial.print("AI: "); Serial.print(CLASS_NAMES[pred]);
  Serial.print(" p="); Serial.println(probs[pred], 2);

  if (pred == lastPred) streak++; else { lastPred = pred; streak = 1; }
  if (streak >= 3 && pred != confirmed) {
    confirmed = pred;
    Serial.print("STATUS: "); Serial.print(CLASS_NAMES[pred]);
    Serial.println(" confirmed");
  }

  int code = STATUS_CODE[pred];
  showStatus(CLASS_NAMES[pred], probs[pred], code);

  if (!radioBusy) {
    sendPayload(code, probs[pred]);
  } else {
    Serial.println("Radio busy, skipped this window's TX");
  }
}

// ---------------- setup / loop ----------------
void setup() {
  // 1. Init board the Heltec way — handles VEXT, SPI, LoRa power internally
  boardInit(LORA_DEBUG_ENABLE, LORA_DEBUG_SERIAL_NUM, 115200);

  // 2. Display power — after boardInit so Vext is already ON
  pinMode(TFT_VDD_EN, OUTPUT);  digitalWrite(TFT_VDD_EN, LOW);
  pinMode(TFT_LEDA_EN, OUTPUT); digitalWrite(TFT_LEDA_EN, LOW);
  delay(200);

  // 3. Display on SPI1
  SPI1.setPins(TFT_MISO, TFT_SCK, TFT_MOSI);
  SPI1.begin();
  tft.init(135, 240);
  tft.setRotation(3);
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 10);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.println("--- SENSOR NODE ---");
  tft.setCursor(0, 40);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("LoRa init...");

  // 4. Radio init — same as the working example
  RadioEvents.TxDone    = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;

  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(
    MODEM_LORA,
    TX_OUTPUT_POWER,
    0,
    LORA_BANDWIDTH,
    LORA_SPREADING_FACTOR,
    LORA_CODINGRATE,
    LORA_PREAMBLE_LENGTH,
    LORA_FIX_LENGTH_PAYLOAD_ON,
    true, 0, 0,
    LORA_IQ_INVERSION_ON,
    3000
  );

  tft.setCursor(0, 70);
  tft.setTextColor(ST77XX_GREEN);
  tft.println("LoRa: OK!");

  // 5. AI sensor init
  haveSensor = adxlInit();
  Serial.println("HELLO T114 AI");
  Serial.println(haveSensor ? "ADXL355 detected." : "ADXL355 NOT detected (SIM mode still works).");
  Serial.println("LoRa: ready");
  Serial.print("Model: "); Serial.print(N_TREES); Serial.print(" trees, ");
  Serial.print(N_NODES); Serial.println(" nodes.");

  rateStart = millis(); lastTick = micros();
}

void loop() {
  // Required — processes LoRa IRQs and handles low power, every cycle
  TimerLowPowerHandler();
  Radio.IrqProcess();

  pollSerial();   // checks for MODE: / RPM: / D: commands

  if (mode == SENSOR_MODE) {
    // 1 kHz real-sensor sampling, non-blocking so it never gets delayed by TX
    uint32_t now = micros();
    if (now - lastTick >= SAMPLE_US) {
      lastTick += SAMPLE_US;
      if (haveSensor) { readSensor(); sampleCount++; addSample(gx, gy, gz); }
    }
  }
  // in SIM_MODE, samples arrive from pollSerial()/parseLine() via "D:" lines instead

  if (millis() - rateStart >= 1000) {
    lastRate = sampleCount; sampleCount = 0; rateStart += 1000;
    Serial.print("OK:"); Serial.println(lastRate);
    const char *src = (mode == SIM_MODE)
        ? ((millis() - lastRx > 500) ? "SIM(no data)" : "SIM")
        : (haveSensor ? "SENSOR" : "SENSOR(none)");
    Serial.print("S:"); Serial.print(src);
    Serial.print(" X:"); Serial.print(gx, 4);
    Serial.print(" Y:"); Serial.print(gy, 4);
    Serial.print(" Z:"); Serial.println(gz, 4);
  }
}

void OnTxDone(void) {
  Serial.println("TX Done!");
  radioBusy = false;
}

void OnTxTimeout(void) {
  Radio.Sleep();
  Serial.println("TX Timeout!");
  radioBusy = false;
}
