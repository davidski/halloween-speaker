// Wiring probe: reads VS1053 registers once a second so you can fix wires live.
// Build/flash with: pio run -e probe -t upload -t monitor
#include <Arduino.h>
#include <SPI.h>
#include "pins.h"

static const uint8_t SCI_READ = 0x03;
static const uint8_t SCI_MODE = 0x00;
static const uint8_t SCI_STATUS = 0x01;

uint16_t sciRead(uint8_t reg) {
  SPI.beginTransaction(SPISettings(250000, MSBFIRST, SPI_MODE0));  // slow clock tolerates long jumper wires
  digitalWrite(VS1053_CS, LOW);
  SPI.transfer(SCI_READ);
  SPI.transfer(reg);
  uint16_t v = SPI.transfer16(0xFFFF);
  digitalWrite(VS1053_CS, HIGH);
  SPI.endTransaction();
  return v;
}

void setup() {
  Serial.begin(115200);
  while (!Serial && millis() < 3000) delay(10);
  // Every chip select idles high so only the VS1053 control port talks on the shared bus
  for (int pin : {VS1053_CS, VS1053_DCS, CARDCS}) { pinMode(pin, OUTPUT); digitalWrite(pin, HIGH); }
  pinMode(VS1053_DREQ, INPUT);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  Serial.println("\nVS1053 wiring probe. Expect MODE=0x4800 (datasheet reset value), STATUS version=4, DREQ=1");
}

void loop() {
  uint16_t mode = sciRead(SCI_MODE);
  uint16_t status = sciRead(SCI_STATUS);
  int version = (status >> 4) & 0x0F;
  int dreq = digitalRead(VS1053_DREQ);

  const char* verdict;
  if (mode == 0xFFFF && status == 0xFFFF) verdict = "all 1s: MISO not connected, or CS/SCK/power missing";
  else if (mode == 0x0000 && status == 0x0000) verdict = "all 0s: MISO shorted low, or wing unpowered (check 3V/GND)";
  else if (version == 4) verdict = "OK: VS1053 answering";
  else verdict = "garbage: check MOSI/SCK, swapped MO/MI, or a loose wire";

  Serial.printf("MODE=0x%04X STATUS=0x%04X version=%d DREQ=%d  -> %s%s\n",
                mode, status, version, dreq, verdict,
                (version == 4 && !dreq) ? " (DREQ low: check A2 -> wing 9)" : "");
  delay(1000);
}
