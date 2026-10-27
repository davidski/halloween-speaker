// Feather ESP32-S3 (#5477) + stacked Music Maker FeatherWing (see DESIGN.md "Pin mapping")
// On this Feather the GPIO numbers equal the silkscreen labels the wing uses.
#ifndef PINS_H
#define PINS_H

#define SPI_SCK      36
#define SPI_MISO     37
#define SPI_MOSI     35
#define VS1053_RESET -1
#define VS1053_CS     6  // D6, MP3CS
#define VS1053_DCS   10  // D10, XDCS
#define VS1053_DREQ   9  // D9
#define CARDCS        5  // D5, SDCS

#endif
