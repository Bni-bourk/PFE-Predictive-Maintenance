#include "Arduino.h"
#include "heltec_nrf_lorawan.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

#define TFT_CS        11
#define TFT_DC        12
#define TFT_RST       2
#define TFT_SCK       40
#define TFT_MOSI      41
#define TFT_MISO      43
#define TFT_VDD_EN    3
#define TFT_LEDA_EN   15

#define RF_FREQUENCY                868000000  
#define TX_OUTPUT_POWER             14         
#define LORA_BANDWIDTH              0          
#define LORA_SPREADING_FACTOR       7          
#define LORA_CODINGRATE             1          
#define LORA_PREAMBLE_LENGTH        8          
#define LORA_FIX_LENGTH_PAYLOAD_ON  false
#define LORA_IQ_INVERSION_ON        false
#define BUFFER_SIZE                 128 // Increased for JSON payload

// --- NODE CONFIGURATION ---
String NODE_ID = "M1"; 
int simBattery = 92;

Adafruit_ST7789 tft = Adafruit_ST7789(&SPI1, TFT_CS, TFT_DC, TFT_RST);
char txpacket[BUFFER_SIZE];
static RadioEvents_t RadioEvents;

typedef enum { LOWPOWER, STATE_TX } States_t;
States_t state;

void OnTxDone(void) { state = STATE_TX; }
void OnTxTimeout(void) { state = STATE_TX; }

void setup() {
  Serial.begin(115200);

  boardInit(LORA_DEBUG_ENABLE, LORA_DEBUG_SERIAL_NUM, 115200);

  pinMode(TFT_VDD_EN, OUTPUT); digitalWrite(TFT_VDD_EN, LOW); 
  pinMode(TFT_LEDA_EN, OUTPUT); digitalWrite(TFT_LEDA_EN, LOW); 
  delay(200);
  
  SPI1.setPins(TFT_MISO, TFT_SCK, TFT_MOSI);
  SPI1.begin(); 
  
  tft.init(135, 240); 
  tft.setRotation(1); 
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Node Init...");

  RadioEvents.TxDone = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;
  Radio.Init(&RadioEvents);
  Radio.SetPublicNetwork(false); 
  Radio.SetChannel(RF_FREQUENCY);
  Radio.SetTxConfig(MODEM_LORA, TX_OUTPUT_POWER, 0, LORA_BANDWIDTH, LORA_SPREADING_FACTOR, LORA_CODINGRATE, LORA_PREAMBLE_LENGTH, LORA_FIX_LENGTH_PAYLOAD_ON, true, 0, 0, LORA_IQ_INVERSION_ON, 3000);

  tft.println("LoRa JSON: OK");
  state = STATE_TX;
}

void loop() {
  switch (state) {
    case STATE_TX: {
      delay(1000); // 1-second update rate

      // 1. Simulate ADXL355 Data (Fluctuates between 2.30 and 2.50 g)
      float simValue = 2.40 + (random(-10, 10) / 100.0);
      
      // 2. Determine Status based on simulated value
      String machineStatus = "NORMAL";
      if (simValue > 2.48) machineStatus = "ALERTE";

      // 3. Build JSON Payload
      // Format: {"id":"M1","st":"NORMAL","v":2.45,"b":92}
      String payload = "{\"id\":\"" + NODE_ID + "\",\"st\":\"" + machineStatus + "\",\"v\":" + String(simValue, 2) + ",\"b\":" + String(simBattery) + "}";
      payload.toCharArray(txpacket, BUFFER_SIZE);

      // Update Screen
      tft.fillScreen(ST77XX_BLACK);
      tft.setCursor(0, 10); tft.setTextColor(ST77XX_CYAN); tft.println("--- NODE " + NODE_ID + " ---");
      tft.setCursor(0, 45); tft.setTextColor(ST77XX_WHITE); tft.print("Vib: "); tft.println(String(simValue, 2) + " g");
      tft.setCursor(0, 80); tft.setTextColor(ST77XX_YELLOW); tft.println("Sending JSON...");

      Serial.println("TX: " + payload);
      Radio.Send((uint8_t *)txpacket, strlen(txpacket));
      
      state = LOWPOWER;
      break;
    }
    case LOWPOWER:
      TimerLowPowerHandler();
      Radio.IrqProcess();
      break;
  }
}