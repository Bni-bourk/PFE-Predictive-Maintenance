/*
 * T114_vibe_ai_lora.ino  -- v2
 * =====================================================================
 * Changes vs v1:
 *  - OVERLAPPING AI windows: verdict every 0.5 s on the last full second
 *    of data (was: every 1 s) -> confirmed diagnosis in ~1.5 s.
 *  - LoRa heartbeat every 1 s (was 2 s); status changes still sent instantly.
 *  - NO-DATA HONESTY: if there's no sensor and no fresh simulator stream,
 *    the node reports st:"OFFLINE" (string) instead of a health code.
 *    Gateway (unchanged!) shows a red OFFLINE badge and writes 0 to Hreg0,
 *    while Hreg4/RSSI stays live -> "link alive, no data" is distinguishable
 *    from "link dead".
 *
 * Sketch folder must contain vibe_ai.h and model_data.h (unchanged).
 * Board: Heltec nRF52 -> Mesh Node T114. Radio: 868 MHz / SF9 / 250 kHz.
 * =====================================================================
 */

#include "Arduino.h"
#include "heltec_nrf_lorawan.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include "vibe_ai.h"

// ---------------- Display ----------------
#define TFT_CS 11
#define TFT_DC 12
#define TFT_RST 2
#define TFT_SCK 40
#define TFT_MOSI 41
#define TFT_MISO 43
#define TFT_VDD_EN 3
#define TFT_LEDA_EN 15
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI1, TFT_CS, TFT_DC, TFT_RST);

// ---------------- LoRa ----------------
#define RF_FREQUENCY               868000000
#define TX_OUTPUT_POWER            22
#define LORA_BANDWIDTH             1
#define LORA_SPREADING_FACTOR      9
#define LORA_CODINGRATE            1
#define LORA_PREAMBLE_LENGTH       8
#define LORA_FIX_LENGTH_PAYLOAD_ON false
#define LORA_IQ_INVERSION_ON       false
#define LORA_TX_PERIOD_MS          1000      // faster heartbeat
static RadioEvents_t RadioEvents;
char txpacket[96];
volatile bool txBusy = false;
uint32_t txCount = 0, txFails = 0;
bool sendNow = false;

// ---------------- sampling ----------------
const uint32_t SAMPLE_HZ = 1000;
const uint32_t SAMPLE_US = 1000000UL / SAMPLE_HZ;

// ---------------- ADXL355 ----------------
const int PIN_CS = 44, PIN_SCK = 47, PIN_MOSI = 10, PIN_MISO = 33;
const uint8_t REG_DEVID_AD = 0x00, REG_XDATA3 = 0x08;
const uint8_t REG_FILTER = 0x28, REG_RANGE = 0x2C, REG_POWER_CTL = 0x2D, REG_RESET = 0x2F;
const uint8_t RANGE_2G = 0x01, ODR_1000HZ = 0x02;
const float   LSB_PER_G = 256000.0f;

// ---------------- state ----------------
enum { SENSOR = 0, SIM = 1 };
int mode = SENSOR;
bool haveSensor = false;
float gx = 0, gy = 0, gz = 0;
float rpmSetting = 1800.0f;
uint32_t sampleCount = 0, lastRate = 0, rateStart = 0, lastTick = 0, lastRx = 0;
uint32_t lastLora = 0, lastDraw = 0;
char line[48]; int lineLen = 0;

// AI ring buffer with overlap: verdict every AI_HOP new samples
#define AI_HOP (AI_WIN / 2)                  // 512 -> every ~0.5 s
static float rxb[AI_WIN], ryb[AI_WIN], rzb[AI_WIN];   // ring
static float lx[AI_WIN], ly[AI_WIN], lz[AI_WIN];      // linearized scratch
int   rpos = 0;
uint32_t totalSamples = 0, sinceAI = 0;
int   lastPred = -1, streak = 0, confirmed = -1;
float lastConf = 0.0f;
const float CONF_MIN = 0.60f;
bool  dataValid = false, prevDataValid = false;

// ---------------- ADXL software SPI ----------------
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

// ---------------- gateway status ----------------
int statusCode() {
  if (confirmed < 0) return 1;
  const char *n = CLASS_NAMES[confirmed];
  if (!strcmp(n, "healthy"))       return 1;
  if (!strcmp(n, "imbalance"))     return 2;
  if (!strcmp(n, "misalignment"))  return 3;
  if (!strcmp(n, "looseness"))     return 4;
  if (!strcmp(n, "bearing_fault")) return 5;
  return 1;
}

// ---------------- LoRa ----------------
void OnTxDone(void)    { txBusy = false; txCount++; }
void OnTxTimeout(void) { Radio.Sleep(); txBusy = false; txFails++; }

void sendLoRa() {
  if (txBusy) return;
  if (dataValid) {
    snprintf(txpacket, sizeof(txpacket),
             "{\"id\":\"M1\",\"st\":%d,\"vx\":%.3f,\"vy\":%.3f,\"vz\":%.3f,\"b\":100}",
             statusCode(), gx, gy, gz);
  } else {
    // no sensor + no fresh sim stream: heartbeat proves the link is alive,
    // but the status is honestly OFFLINE (string -> gateway writes Hreg0 = 0)
    snprintf(txpacket, sizeof(txpacket),
             "{\"id\":\"M1\",\"st\":\"OFFLINE\",\"vx\":0,\"vy\":0,\"vz\":0,\"b\":100}");
  }
  txBusy = true;
  Radio.Send((uint8_t *)txpacket, strlen(txpacket));
}

// ---------------- AI ----------------
void runAI() {
  // linearize the ring (oldest -> newest) into scratch
  for (int i = 0; i < AI_WIN; i++) {
    int idx = (rpos + i) % AI_WIN;
    lx[i] = rxb[idx]; ly[i] = ryb[idx]; lz[i] = rzb[idx];
  }
  float feats[N_FEATURES], probs[N_CLASSES];
  ai_extract_features(lx, ly, lz, rpmSetting, feats);
  int pred = ai_predict(feats, probs);
  lastConf = probs[pred];

  if (probs[pred] < CONF_MIN) {
    Serial.print("AI: uncertain (best guess ");
    Serial.print(CLASS_NAMES[pred]);
    Serial.print(" p="); Serial.print(probs[pred], 2); Serial.println(")");
    lastPred = -1; streak = 0;
    return;
  }
  Serial.print("AI: "); Serial.print(CLASS_NAMES[pred]);
  Serial.print(" p="); Serial.println(probs[pred], 2);

  if (pred == lastPred) streak++; else { lastPred = pred; streak = 1; }
  if (streak >= 3 && pred != confirmed) {
    confirmed = pred;
    Serial.print("STATUS: "); Serial.print(CLASS_NAMES[pred]);
    Serial.println(" confirmed");
    sendNow = true;
  }
}
void addSample(float x, float y, float z) {
  rxb[rpos] = x; ryb[rpos] = y; rzb[rpos] = z;
  rpos = (rpos + 1) % AI_WIN;
  totalSamples++; sinceAI++;
  if (totalSamples >= AI_WIN && sinceAI >= AI_HOP) { sinceAI = 0; runAI(); }
}
void resetAI() {
  totalSamples = 0; sinceAI = 0; rpos = 0;
  lastPred = -1; streak = 0; confirmed = -1; lastConf = 0;
}

// ---------------- serial protocol ----------------
void parseLine(char *s) {
  if (!strncmp(s, "MODE:", 5)) {
    mode = strstr(s, "SIM") ? SIM : SENSOR;
    sampleCount = 0; rateStart = millis();
    resetAI(); sendNow = true;
    Serial.println(mode == SIM ? "MODE SET: SIMULATOR" : "MODE SET: REAL SENSOR");
  } else if (!strncmp(s, "RPM:", 4)) {
    float r = atof(s + 4);
    if (r >= 60 && r <= 12000) rpmSetting = r;
    Serial.print("RPM SET: "); Serial.println(rpmSetting, 0);
  } else if (s[0] == 'D' && s[1] == ':') {
    if (mode == SIM) {
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

// ---------------- display ----------------
void tftField(int x, int y, uint8_t size, uint16_t color, const char *txt) {
  tft.setTextSize(size);
  tft.setTextColor(color, ST77XX_BLACK);
  tft.setCursor(x, y);
  tft.print(txt);
}
void drawStatic() {
  tft.fillScreen(ST77XX_BLACK);
  tftField(4, 2, 2, ST77XX_CYAN, "PdM NODE M1");
}
void updateDisplay() {
  char buf[32];
  snprintf(buf, sizeof(buf), "SRC:%-10s %4luHz",
           mode == SIM ? (dataValid ? "SIMULATOR" : "SIM(wait)")
                       : (haveSensor ? "SENSOR" : "NO SENSOR"),
           (unsigned long)lastRate);
  tftField(4, 24, 1, ST77XX_WHITE, buf);

  snprintf(buf, sizeof(buf), "X %+7.3f g   ", gx); tftField(4, 38, 1, ST77XX_CYAN,   buf);
  snprintf(buf, sizeof(buf), "Y %+7.3f g   ", gy); tftField(4, 50, 1, ST77XX_YELLOW, buf);
  snprintf(buf, sizeof(buf), "Z %+7.3f g   ", gz); tftField(4, 62, 1, ST77XX_GREEN,  buf);

  const char *lab; uint16_t col;
  if (!dataValid)          { lab = "NO DATA";     col = ST77XX_ORANGE; }
  else if (confirmed < 0)  { lab = "warming up";  col = ST77XX_WHITE;  }
  else { lab = CLASS_NAMES[confirmed];
         col = (statusCode() == 1) ? ST77XX_GREEN : ST77XX_RED; }
  snprintf(buf, sizeof(buf), "%-13s", lab);
  tftField(4, 80, 2, col, buf);
  snprintf(buf, sizeof(buf), "conf %.2f  code %d ", lastConf,
           dataValid ? statusCode() : 0);
  tftField(4, 100, 1, ST77XX_WHITE, buf);

  snprintf(buf, sizeof(buf), "LoRa TX:%lu fail:%lu %s ",
           (unsigned long)txCount, (unsigned long)txFails, txBusy ? ">" : " ");
  tftField(4, 118, 1, ST77XX_ORANGE, buf);
}

// ---------------- setup / loop ----------------
void setup() {
  boardInit(LORA_DEBUG_ENABLE, LORA_DEBUG_SERIAL_NUM, 115200);

  pinMode(TFT_VDD_EN, OUTPUT);  digitalWrite(TFT_VDD_EN, LOW);
  pinMode(TFT_LEDA_EN, OUTPUT); digitalWrite(TFT_LEDA_EN, LOW);
  delay(200);
  SPI1.setPins(TFT_MISO, TFT_SCK, TFT_MOSI);
  SPI1.begin();
  tft.init(135, 240);
  tft.setRotation(3);
  drawStatic();
  tftField(4, 38, 1, ST77XX_WHITE, "LoRa init...");

  RadioEvents.TxDone    = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  Radio.Init(&RadioEvents);
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH,
                    LORA_SPREADING_FACTOR, LORA_CODINGRATE, LORA_PREAMBLE_LENGTH,
                    LORA_FIX_LENGTH_PAYLOAD_ON, true, 0, 0,
                    LORA_IQ_INVERSION_ON, 3000);
  tftField(4, 38, 1, ST77XX_GREEN, "LoRa: OK      ");

  haveSensor = adxlInit();
  Serial.println("HELLO T114 AI+LORA v2");
  Serial.println(haveSensor ? "ADXL355 detected." : "ADXL355 NOT detected (SIM mode still works).");

  rateStart = millis(); lastTick = micros(); lastLora = millis();
}

void loop() {
  pollSerial();
  Radio.IrqProcess();

  if (mode == SENSOR) {
    uint32_t now = micros();
    if (now - lastTick >= SAMPLE_US) {
      lastTick += SAMPLE_US;
      if (haveSensor) { readSensor(); sampleCount++; addSample(gx, gy, gz); }
    }
  }

  uint32_t now = millis();

  // does the node actually have a data source right now?
  dataValid = (mode == SENSOR) ? haveSensor : (now - lastRx < 1000);
  if (dataValid != prevDataValid) {
    prevDataValid = dataValid;
    if (!dataValid) resetAI();     // don't keep a stale diagnosis
    sendNow = true;                // push the transition to the gateway now
  }

  if (now - rateStart >= 1000) {
    lastRate = sampleCount; sampleCount = 0; rateStart += 1000;
    Serial.print("OK:"); Serial.println(lastRate);
    const char *src = (mode == SIM)
        ? (dataValid ? "SIM" : "SIM(no data)")
        : (haveSensor ? "SENSOR" : "SENSOR(none)");
    Serial.print("S:"); Serial.print(src);
    Serial.print(" X:"); Serial.print(gx, 4);
    Serial.print(" Y:"); Serial.print(gy, 4);
    Serial.print(" Z:"); Serial.println(gz, 4);
  }

  if (sendNow || (now - lastLora >= LORA_TX_PERIOD_MS)) {
    sendNow = false; lastLora = now;
    sendLoRa();
  }

  if (now - lastDraw >= 250) { lastDraw = now; updateDisplay(); }
}