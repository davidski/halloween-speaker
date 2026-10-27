# Agent notes: Halloween speaker

## This is a learning project

The owner is using this project to learn embedded development (ESP32, SPI, PlatformIO, MQTT, Home Assistant). Getting it working matters, but understanding it matters just as much.

- When you change something, explain **why** in a sentence or two: the concept behind it, not only the diff. Example: "These are strapping pins; the chip reads them at reset to choose its boot mode, so a peripheral holding them high can stop it booting."
- Point out teachable moments as they come up: a datasheet detail, a debugging technique, a common mistake, or how to verify a claim (which file or doc to check).
- Show how you found an answer (the variant `pins_arduino.h`, the Adafruit guide, the library source) so the owner can find it next time.
- For small, well-scoped steps the owner could do themselves, offer the hint first and let them choose whether to do it or have you do it.
- Keep explanations short and concrete. Don't lecture, and don't hold back a fix that is needed.

## Hardware

- Board: **Adafruit Feather ESP32-S3, 4MB flash, 2MB PSRAM (#5477)** (PlatformIO `adafruit_feather_esp32s3`), with a stacked Music Maker FeatherWing (#3436). Pins live in `include/pins.h`. The earlier hand-wired QT Py ESP32-S3 build is documented in DESIGN.md as "Previous build".
- DESIGN.md is the source of truth for wiring and decisions. Take GPIO numbers from the arduino-esp32 variant file, and never guess them.
