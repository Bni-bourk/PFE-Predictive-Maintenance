#include "Arduino.h"
#include "heltec_nrf_lorawan.h"
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// Display Pins (SPI1)
#define TFT_CS        11
#define TFT_DC        12
#define TFT_RST       2
#define TFT_SCK       40
#define TFT_MOSI      41
#define TFT_MISO      43
#define TFT_VDD_EN    3
#define TFT_LEDA_EN   15

// LoRa Settings — same as working example
#define RF_FREQUENCY                868000000  // 868 MHz for Europe
#define TX_OUTPUT_POWER             5          // dBm
#define LORA_BANDWIDTH              0          // 125 kHz
#define LORA_SPREADING_FACTOR       7
#define LORA_CODINGRATE             1          // 4/5
#define LORA_PREAMBLE_LENGTH        8
#define LORA_FIX_LENGTH_PAYLOAD_ON  false
#define LORA_IQ_INVERSION_ON        false
#define BUFFER_SIZE                 64

// Display
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI1, TFT_CS, TFT_DC, TFT_RST);

// Radio
char txpacket[BUFFER_SIZE];
static RadioEvents_t RadioEvents;

typedef enum { LOWPOWER, STATE_TX } States_t;
States_t state;

// Forward declarations
void OnTxDone(void);
void OnTxTimeout(void);

// =====================
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

  // 4. Radio init — exactly like the working example
  RadioEvents.TxDone    = OnTxDone;
  RadioEvents.TxTimeout = OnTxTimeout;

  Radio.Init(&RadioEvents);
  Radio.SetPublicNetwork(false);  // ADD THIS LINE
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

  state = STATE_TX;
}

// =====================
void loop() {
  switch (state) {

    case STATE_TX: {
      delay(5000);

      String payload = "M1: NORMAL";
      payload.toCharArray(txpacket, BUFFER_SIZE);

      // Update display
      tft.fillScreen(ST77XX_BLACK);
      tft.setCursor(0, 10);
      tft.setTextColor(ST77XX_CYAN);
      tft.setTextSize(2);
      tft.println("--- SENSOR NODE ---");
      tft.setCursor(0, 45);
      tft.setTextColor(ST77XX_WHITE);
      tft.print("Status: ");
      tft.setTextColor(ST77XX_GREEN);
      tft.println(payload);
      tft.setCursor(0, 80);
      tft.setTextColor(ST77XX_YELLOW);
      tft.println("Transmitting...");

      Serial.printf("Sending: \"%s\"\n", txpacket);
      Radio.Send((uint8_t *)txpacket, strlen(txpacket));
      state = LOWPOWER;
      break;
    }

    case LOWPOWER:
      // Required — processes IRQ and handles low power
      TimerLowPowerHandler();
      Radio.IrqProcess();
      break;

    default:
      break;
  }
}

// =====================
void OnTxDone(void) {
  Serial.println("TX Done!");
  tft.setCursor(0, 110);
  tft.setTextColor(ST77XX_GREEN);
  tft.println("Sent OK!");
  state = STATE_TX;
}

void OnTxTimeout(void) {
  Radio.Sleep();
  Serial.println("TX Timeout!");
  tft.setCursor(0, 110);
  tft.setTextColor(ST77XX_RED);
  tft.println("TX Timeout!");
  state = STATE_TX;
}