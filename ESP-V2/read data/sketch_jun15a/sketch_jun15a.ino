#include <SPI.h>

// --- FOOLPROOF LEFT RAIL PINS ---
const int CS_PIN   = 28;
const int MOSI_PIN = 29;
const int MISO_PIN = 30;
const int SCK_PIN  = 31;

// Create custom SPI bus for the nRF52840
SPIClass adxlSPI(NRF_SPIM2, MISO_PIN, SCK_PIN, MOSI_PIN);

// --- ADXL355 REGISTERS ---
#define REG_DEVID_AD   0x00  // Analog Devices ID
#define REG_DEVID_MST  0x01  // MEMS ID
#define REG_XDATA3     0x08  // Roll-over start register for X, Y, Z data
#define REG_POWER_CTL  0x2D  // Power control register

// --- SPI COMMANDS ---
#define SPI_READ       0x01
#define SPI_WRITE      0x00

// ADXL355 supports up to 5MHz, but 1MHz is incredibly stable for wiring
SPISettings adxlSpiSettings(1000000, MSBFIRST, SPI_MODE0);

void setup() {
  Serial.begin(115200);
  
  uint32_t t = millis();
  while (!Serial && (millis() - t < 3000)); 

  Serial.println("\n--- ADXL355 3-Axis Data Reader ---");

  // 1. SETUP CHIP SELECT (CS) PIN
  pinMode(CS_PIN, OUTPUT);
  digitalWrite(CS_PIN, HIGH);

  // 2. START THE SPI BUS
  adxlSPI.begin();
  delay(100);

  // 3. VERIFY CONNECTION
  uint8_t devIdAd = readRegister(REG_DEVID_AD);
  if (devIdAd != 0xAD) {
    Serial.println("FAIL: Sensor disconnected. Check wires.");
    while (1); // Halt execution
  }
  Serial.println("SUCCESS: Sensor found! Booting up...");

  // 4. WAKE UP THE SENSOR
  // The ADXL355 powers on in "Standby" mode. 
  // We write 0x00 to the POWER_CTL register to switch it into Measurement Mode.
  writeRegister(REG_POWER_CTL, 0x00);
  delay(100); // Give the MEMS elements a moment to stabilize
}

void loop() {
  int32_t rawX, rawY, rawZ;
  
  // Read the raw 20-bit data registers
  read3AxisData(&rawX, &rawY, &rawZ);

  // Convert raw data to gravity (g)
  // Assuming default +/- 2g range. The scale factor is 3.9 micro-g per LSB.
  float scaleFactor = 0.0000039;
  float accX = rawX * scaleFactor;
  float accY = rawY * scaleFactor;
  float accZ = rawZ * scaleFactor;

  // Print formatted results
  Serial.print("X: "); Serial.print(accX, 4); Serial.print(" g \t|\t ");
  Serial.print("Y: "); Serial.print(accY, 4); Serial.print(" g \t|\t ");
  Serial.print("Z: "); Serial.print(accZ, 4); Serial.println(" g");
  
  // Read 10 times a second
  delay(100); 
}

// ==========================================
// SPI DRIVER FUNCTIONS
// ==========================================

uint8_t readRegister(uint8_t regAddress) {
  uint8_t value = 0;
  // Shift address left by 1 and set the lowest bit to 1 for "READ"
  uint8_t commandByte = (regAddress << 1) | SPI_READ;

  adxlSPI.beginTransaction(adxlSpiSettings);
  digitalWrite(CS_PIN, LOW);

  adxlSPI.transfer(commandByte);
  value = adxlSPI.transfer(0x00); 

  digitalWrite(CS_PIN, HIGH);
  adxlSPI.endTransaction();
  
  return value;
}

void writeRegister(uint8_t regAddress, uint8_t value) {
  // Shift address left by 1 and set the lowest bit to 0 for "WRITE"
  uint8_t commandByte = (regAddress << 1) | SPI_WRITE;

  adxlSPI.beginTransaction(adxlSpiSettings);
  digitalWrite(CS_PIN, LOW);

  adxlSPI.transfer(commandByte);
  adxlSPI.transfer(value);

  digitalWrite(CS_PIN, HIGH);
  adxlSPI.endTransaction();
}

void read3AxisData(int32_t *x, int32_t *y, int32_t *z) {
  uint8_t buffer[9];
  uint8_t commandByte = (REG_XDATA3 << 1) | SPI_READ;

  adxlSPI.beginTransaction(adxlSpiSettings);
  digitalWrite(CS_PIN, LOW);

  adxlSPI.transfer(commandByte);
  // Burst read 9 sequential registers containing X, Y, and Z data
  for (int i = 0; i < 9; i++) {
    buffer[i] = adxlSPI.transfer(0x00); 
  }

  digitalWrite(CS_PIN, HIGH);
  adxlSPI.endTransaction();

  // The ADXL355 uses 20-bit resolution packed into 3 bytes per axis.
  // We reconstruct those 20 bits and shift them into standard 32-bit integers.
  uint32_t rawX = ((uint32_t)buffer[0] << 12) | ((uint32_t)buffer[1] << 4) | (buffer[2] >> 4);
  uint32_t rawY = ((uint32_t)buffer[3] << 12) | ((uint32_t)buffer[4] << 4) | (buffer[5] >> 4);
  uint32_t rawZ = ((uint32_t)buffer[6] << 12) | ((uint32_t)buffer[7] << 4) | (buffer[8] >> 4);

  // Sign extension: If the 20th bit is a 1 (negative number), pad the rest of the 32-bit integer with 1s.
  if (rawX & 0x80000) rawX |= 0xFFF00000;
  if (rawY & 0x80000) rawY |= 0xFFF00000;
  if (rawZ & 0x80000) rawZ |= 0xFFF00000;

  *x = (int32_t)rawX;
  *y = (int32_t)rawY;
  *z = (int32_t)rawZ;
}