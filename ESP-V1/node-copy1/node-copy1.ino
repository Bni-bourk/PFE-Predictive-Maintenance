#include <Arduino.h>
#include <SPI.h>
#include <RadioLib.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// --- PINS FOR HELTEC MESH NODE T114 (NORDIC nRF52840) ---

// Power Control Pins
#define VEXT_EN       21  // Power for LoRa and Peripherals
#define TFT_VDD_EN    3   // Display Power (Active LOW)
#define TFT_LEDA_EN   15  // Display Backlight (Active LOW)

// Display Pins (SPI1)
#define TFT_CS        11  // P0.11
#define TFT_DC        12  // P0.12
#define TFT_RST       2   // P0.02
#define TFT_SCK       40  // P1.08
#define TFT_MOSI      41  // P1.09
#define TFT_MISO      43  // P1.11 (NC)

// LoRa Pins (SPI)
#define LORA_NSS      35  // P1.03
#define LORA_DIO1     39  // P1.07
#define LORA_NRST     25  // P0.25 (Fixed from 38)
#define LORA_BUSY     36  // P1.04
#define LORA_SCK      32  // P1.00
#define LORA_MISO     34  // P1.02
#define LORA_MOSI     33  // P1.01

// Initialize Display (ST7789)
// T114 uses SPI1 for the display
Adafruit_ST7789 tft = Adafruit_ST7789(&SPI1, TFT_CS, TFT_DC, TFT_RST);

// Initialize LoRa
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_NRST, LORA_BUSY);

void setup() {
  Serial.begin(115200);

  // 1. Power ON sequence for T114
  pinMode(VEXT_EN, OUTPUT);
  digitalWrite(VEXT_EN, HIGH);    // Enable Vext (Powers LoRa & Sensors)
  
  pinMode(TFT_VDD_EN, OUTPUT);
  digitalWrite(TFT_VDD_EN, LOW);  // Enable Display Power
  
  pinMode(TFT_LEDA_EN, OUTPUT);
  digitalWrite(TFT_LEDA_EN, LOW); // Enable Backlight
  
  delay(500); // Give it more time to wake up

  // 2. Initialize Screen on SPI1
  SPI1.setPins(TFT_MISO, TFT_SCK, TFT_MOSI);
  SPI1.begin();
  
  tft.init(135, 240);
  tft.setRotation(3); 
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 20);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.println(" T114 Node Init...");

  // 3. Initialize LoRa on SPI
  SPI.setPins(LORA_MISO, LORA_SCK, LORA_MOSI);
  SPI.begin();
  
  // 4. Initialize LoRa (868.0 MHz)
  // T114 V1 has interference issues. Using low power (2dBm) and LDO mode helps avoid -705 errors.
  int state = radio.begin(868.0, 125.0, 7, 5, 0x12, 2, 8, 1.6, true);
  
  tft.setCursor(0, 50);
  if (state == RADIOLIB_ERR_NONE) {
    tft.setTextColor(ST77XX_GREEN);
    tft.println(" LoRa: SUCCESS");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.println(" LoRa: FAILED");
    tft.print(" Code: ");
    tft.println(state);
    // If it still fails with -2, let's try a different Reset pin or check Vext
  }
  delay(2000);
}

void loop() {
  String status = "M1: NORMAL";
  
  // Show on Screen
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 10);
  tft.setTextColor(ST77XX_CYAN);
  tft.setTextSize(2);
  tft.println("--- SENSOR NODE ---");
  
  tft.setCursor(0, 50);
  tft.setTextColor(ST77XX_WHITE);
  tft.print("Status: ");
  tft.setTextColor(ST77XX_GREEN);
  tft.println(status);
  
  tft.setCursor(0, 90);
  tft.setTextColor(ST77XX_YELLOW);
  tft.println("Transmitting...");

  // Send over LoRa
  Serial.print("Transmitting: "); Serial.println(status);
  int state = radio.transmit(status);

  tft.setCursor(0, 115);
  if (state == RADIOLIB_ERR_NONE) {
    tft.setTextColor(ST77XX_GREEN);
    tft.println("Sent OK!");
    Serial.println("Transmission successful!");
  } else {
    tft.setTextColor(ST77XX_RED);
    tft.print("Sent ERR: ");
    tft.println(state);
    Serial.print("Transmission failed, code: "); Serial.println(state);
  }

  delay(5000); // Wait 5 seconds
}

