# Halloween Speaker Sound Effects

When someone physically presses the Amcrest AD410 doorbell, a speaker by the front door plays a random spooky sound. An ESP32 drives an Adafruit Music Maker FeatherWing, and Home Assistant triggers it over MQTT.

- **Deadline:** Halloween night, October 31. The install is temporary.
- **Constraints:** reuse existing hardware, run fully local, keep new purchases to a minimum.

## Existing hardware

| Component | Hardware | Notes |
| --- | --- | --- |
| Doorbell | Amcrest AD410 | Already integrated with Home Assistant |
| Audio decoder | Adafruit Music Maker FeatherWing | VS1053-based |
| Amplifier | Built into the FeatherWing | Stereo Class-D, up to 3 W/channel |
| Speaker | Adafruit #4445 Mono Enclosed Speaker, 4 Ω / 3 W, 70 × 31 × 17 mm, M3 holes on 64 × 31 mm | Matches the amp's rated 3 W/channel into 4 Ω |
| Enclosure | Adafruit #905 Large Weatherproof Project Enclosure, 175 × 125 × 75 mm, IP66, M4 bosses | Everything outdoors under the covered porch |
| Microcontroller | Adafruit Feather ESP32-S3 (#5477, 4MB flash, 2MB PSRAM) | Music Maker FeatherWing stacks on top |
| Automation | Home Assistant | Existing install |
| Messaging | MQTT | Preferred transport |

The FeatherWing has a four-position speaker terminal block, so it is the amplified version, **Adafruit #3436** (confirmed: it has screw terminals and no headphone jack). It provides a VS1053 decoder, microSD storage, a stereo Class-D amp (3 W/channel into 4 Ω), and an SPI host interface. It plays MP3 and other formats.

The 4 Ω / 3 W speaker matches the amp's rating, so there is no headroom. Cap the volume in firmware (VS1053 `setVolume`, where lower numbers are louder). Start around 20 and only go louder after listening for distortion. At full output the amp can draw 1 A or more from the 5 V rail, so use a 2 A supply and short, thick 5 V and GND wires. **The amp outputs are bridge-tied, so never connect either speaker terminal to ground.** 

## Architecture

```text
AD410 button press
      │
      ▼
Home Assistant ── automation (gated by input_boolean.halloween_sounds)
      │  MQTT: halloween/speaker/play
      ▼
ESP32 ── MQTT client, playback controller, random selection
      │  SPI
      ▼
Music Maker FeatherWing ── VS1053 decoder, microSD, stereo amp
      │
      ▼
Passive 4 Ω speaker at the front door
```

Once triggered, playback runs independently of Home Assistant. No audio streaming is involved.

## Firmware

Use the Arduino framework built with PlatformIO, with these libraries: Adafruit VS1053, Adafruit BusIO (if required), PubSubClient or another ESP32 MQTT client, and the ESP32 Wi-Fi libraries. Confirm that the VS1053 library works on ESP32, especially playback and interrupt (DREQ) handling.

**Required behavior**

1. Connect to Wi-Fi, then to the MQTT broker.
2. Subscribe to the command topic and accept play commands.
3. Pick a file from microSD and play it through the VS1053.
4. Return to idle after playback.
5. Reconnect automatically after network drops.

### MQTT interface

| Topic | Direction | Payload |
| --- | --- | --- |
| `halloween/speaker/play` | HA → ESP32, **not retained** | `{"action":"play"}` follows Mode; `"sound":"random"`, `"sound":"next"` (sequential, wraps around), or a file name like `"sound":"moaning-ghost.mp3"` |
| `halloween/speaker/status` | ESP32 → HA | `idle` · `playing` · `error` |

The play topic must not be retained, so a reconnect never replays an old doorbell press.

### Playback rules

- Choose a random file, and avoid repeating the last one when there are alternatives.
- Play one sound at a time. Ignore commands that arrive during playback.
- Apply a configurable cooldown after each sound.
- Publish `error` when a file can't be read.
- The playlist is every `.mp3` in the SD card root, scanned at boot and logged to serial. Skips folders and macOS `._*` metadata files. A specific `"sound"` must match a scanned name (leading `/` optional). Add or rename files, then reboot.

### Preparing sound files

The firmware plays every `.mp3` in the SD card root (see Playback rules). Convert each clip to this format before copying it to the card:

| Setting | Value | Why |
| --- | --- | --- |
| Channels | Mono | One speaker |
| Sample rate | 44.1 kHz | VS1053 native rate |
| Bitrate | 128 kbps | Plenty for a small speaker; small files |
| Loudness | `loudnorm` to −16 LUFS | Even volume between clips, so a quiet line and a scream need the same slider setting |
| True peak | −1.5 dBTP | Headroom so loud clips don't clip at `VOLUME_LOUDEST` 0 |
| Length | 2–10 s | Suits a doorbell |

**Batch:** put source files (wav, mp3, m4a…) in `sounds/raw/` and run `uv run sounds/process.py` (`--max 8` is the default length cap in seconds). It writes `sounds/sd/`: leading silence (below −50 dB) trimmed, capped to `--max` with a 0.5 s fade-out when cut, mono 44.1 kHz 128 kbps, two-pass `loudnorm` (measure, then apply the exact correction). Output names drop the freesound `<id>__<user>__` prefix. Copy `sounds/sd/*.mp3` to the card root.

To hand-pick a section instead, cut and convert one clip (here 0:12.5–0:17):

```
ffmpeg -ss 12.5 -to 17 -i source.m4a -ac 1 -ar 44100 -af loudnorm=I=-16:TP=-1.5 -b:a 128k death01.mp3
```

- `-ss` / `-to`: start and end in seconds (or `hh:mm:ss.x`). Placed before `-i` so ffmpeg seeks quickly.
- `-ac 1`: mono. `-ar 44100`: 44.1 kHz. `-b:a 128k`: bitrate.
- `-af loudnorm=I=-16:TP=-1.5`: normalize loudness and cap peaks.

Convert a whole file with no trimming by dropping `-ss` and `-to`.

After copying files to the card from a Mac, run `dot_clean /Volumes/<card>` to remove the `._*` metadata files. The firmware skips them anyway, but they waste space. Reboot the speaker to rescan the card; the log lists every file it found.

**Sources:** freesound.org (check each sound's CC0 or CC-BY licence), Pixabay sound effects, BBC Sound Effects (personal use), ZapSplat. Character quotes (Discworld's Death, Labyrinth) come from copies you own: audiobooks, DVD/Blu-ray, digital.

### Home Assistant discovery

The device publishes MQTT discovery payloads automatically on every MQTT connection and when it detects Home Assistant coming online. Additional topics:

| Topic | Direction | Content |
| --- | --- | --- |
| `halloween/speaker/availability` | ESP32 → HA | `online` · `offline` (retained) |
| `halloween/speaker/rssi` | ESP32 → HA, every 60 s | Wi-Fi signal in dBm (retained) |
| `halloween/speaker/volume/set` | HA → ESP32, retained by HA | `0`–`100` (%). Replayed by the broker on reconnect, so volume survives reboots |
| `halloween/speaker/volume` | ESP32 → HA, retained | Current volume %. 100% = attenuation `VOLUME_LOUDEST` (5, under test); 1% = 85; 0% = mute |
| `halloween/speaker/mode/set` | HA → ESP32, retained by HA | `random` or `sequential`: what Play does when no `sound` is given |
| `halloween/speaker/mode` | ESP32 → HA, retained | Current mode |
| `halloween/speaker/restart` | HA → ESP32, **not retained** | `restart`: reboot the device |
| `halloween/speaker/log` | ESP32 → anyone, not retained | Playback start/finish, errors, SD recoveries. Watch with `mosquitto_sub -v -t halloween/speaker/log` |
| `homeassistant/status` | HA → ESP32 | Subscribed to trigger discovery republish |

Discovered entities under `homeassistant/sensor/` and `homeassistant/button/`:

- **Status** (`halloween_speaker_status`): state topic = `halloween/speaker/status`, icon `mdi:ghost`
- **Play** button (`halloween_speaker_play`): command topic = `halloween/speaker/play`, payload = `{"action":"play","sound":"random"}`
- **Restart** button (`halloween_speaker_restart`): command topic = `halloween/speaker/restart`, payload = `restart`; command is not retained
- **Wi-Fi Signal** sensor (`halloween_speaker_rssi`): state topic = `halloween/speaker/rssi`, device class `signal_strength`, unit `dBm`

All entities share a single device block (identifiers [DEVICE_ID], name "Halloween Speaker", manufacturer "Adafruit", model "QT Py ESP32-S3 + Music Maker FeatherWing") and use `halloween/speaker/availability` as their availability topic.

## Home Assistant integration

Home Assistant detects the AD410 press and publishes the play command. The ESP32 never talks to the doorbell directly. An `input_boolean.halloween_sounds` toggle turns the effect on or off without a firmware change.

The example below is illustrative. The AD410 may expose presses as an event rather than a binary sensor, so check the trigger against the existing configuration.

```yaml
alias: Halloween Speaker Sounds
triggers:
  - trigger: state
    entity_id: binary_sensor.front_doorbell_button
    to: "on"
conditions:
  - condition: state
    entity_id: input_boolean.halloween_sounds
    state: "on"
actions:
  - action: mqtt.publish
    data:
      topic: halloween/speaker/play
      retain: false
      payload: '{"action":"play","sound":"random"}'
mode: single
```

## Hardware integration

**Board: Adafruit Feather ESP32-S3, 4MB flash / 2MB PSRAM (#5477)** (decided 2026-10-20; replaced the hand-wired QT Py ESP32-S3). Same chip and memory as the QT Py, but the Music Maker FeatherWing **stacks** on it: female headers on the Feather, male headers on the wing, wing on top. No jumper wires to work loose in an outdoor box.

### Pin mapping: Feather ESP32-S3 + stacked Music Maker FeatherWing

GPIO numbers come from the arduino-esp32 `adafruit_feather_esp32s3` variant (`pins_arduino.h`: A8=5, A9=6, A10=9, A11=10; SPI SCK 36, MOSI 35, MISO 37). They match the library's `examples/feather_player/feather_player.ino` ESP32-S2/S3 defaults. On this Feather, **GPIO number = silkscreen label**. PlatformIO board ID: `adafruit_feather_esp32s3`. Code: `include/pins.h`.

| Wing function | Feather position | GPIO | Code |
| --- | --- | --- | --- |
| SCK / MOSI / MISO | SCK / MO / MI | 36 / 35 / 37 | `SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI)` |
| MP3CS (VS1053 CS) | D6 | 6 | `VS1053_CS` |
| XDCS (data select) | D10 | 10 | `VS1053_DCS` |
| DREQ | D9 | 9 | `VS1053_DREQ`, input |
| SDCS (SD card CS) | D5 | 5 | `CARDCS` |
| RST | RST | — | Shares the Feather reset; `VS1053_RESET -1` |
| USB | USB | — | Amp supply (5V from USB-C) |

- **Strapping pins:** the S3's are GPIO 0, 3, 45 and 46. None are used by the wing.
- **USB:** native USB on the S3 (same as the QT Py): the port re-enumerates on reset, and `ARDUINO_USB_CDC_ON_BOOT=1` routes `Serial` over it. If the port vanishes after a crash, hold BOOT and tap RESET.
- **Flash layout:** the board uses `partitions-4MB-tinyuf2.csv`: two 1408 KB OTA slots, a UF2 bootloader, and a FAT partition.
- **On-board extras:** NeoPixel on GPIO 33 (power on GPIO 21), red LED on GPIO 13, STEMMA QT port (I2C power on GPIO 7).
- **DREQ:** start with polling via `startPlayingFile()` and `feedBuffer()`, not `useInterrupt`.

### Previous build: QT Py ESP32-S3 → Music Maker FeatherWing (hand-wired, retired 2026-10-20)

Kept for reference and for any future hand-wired wing. FeatherWing pin labels do not map one-to-one to QT Py GPIO numbers.

The QT Py's GPIO numbers come from the arduino-esp32 `adafruit_qtpy_esp32s3_n4r2` variant (`pins_arduino.h`). The PlatformIO board ID is `adafruit_qtpy_esp32s3_n4r2`. The wing's pins are silkscreened with standard Feather names; the function → Feather-position mapping comes from the library's `examples/feather_player/feather_player.ino` defaults (CS 6, DCS 10, CARDCS 5, DREQ 9).

| FeatherWing function | Wing silk (Feather label) | QT Py pin (silk) | GPIO | Notes |
| --- | --- | --- | --- | --- |
| SCK | SCK | SCK | 36 | Shared SPI |
| MOSI | MO | MO | 35 | Shared SPI |
| MISO | MI | MI | 37 | Shared SPI |
| MP3CS (VS1053 CS) | 6 | A0 | 18 | `VS1053_CS` |
| XDCS (data select) | 10 | A1 | 17 | `VS1053_DCS` |
| DREQ | 9 | A2 | 9 | `VS1053_DREQ`, input |
| SDCS (SD card CS) | 5 | SDA | 7 | `CARDCS` |
| 3V | 3V | 3V | — | Logic, VS1053 and SD |
| USB | USB | 5V | — | Amp supply |
| GND | GND | GND | — | Common ground. Use at least two wires. |
| RST | RST | not connected | — | Spare SCL/6 if a GPIO reset is needed |
| TX (MIDI in) | TX | not connected | — | |

#### Physical layout (verified 2026-10-15)

FeatherWing top side up, 12-pin header on the left. The 12-pin row is flush with one end of the 16-pin row; that end is drawn at the top. ★ = the six wires the VS1053 needs to answer (`VS1053 OK`).

```
   12-pin (left)                            16-pin (right)
   SDA  ●  (unused)                  NC  ●
   SCL  ●                            TX  ●
     5  ●  ── QT Py SDA (SDCS)       RX  ●
     6  ●  ── QT Py A0  (MP3CS) ★    MI  ●  ── QT Py MI  ★
     9  ●  ── QT Py A2  (DREQ)       MO  ●  ── QT Py MO  ★
    10  ●  ── QT Py A1  (XDCS)      SCK  ●  ── QT Py SCK ★
    11  ●                            A5  ●
    12  ●                            A4  ●
    13  ●                            A3  ●
   USB  ●  ── QT Py 5V  (amp)        A2  ●
    EN  ●                            A1  ●
   BAT  ●                            A0  ●
                                    GND  ●  ── QT Py GND ★
         (12-pin row ends           AREF ●
          4 positions early)         3V  ●  ── QT Py 3V  ★
                                    RST  ●
```

QT Py ESP32-S3, top side up, USB-C at the top:

```
        ┌──USB-C──┐
   A0  ●│         │●  5V
   A1  ●│         │●  GND
   A2  ●│         │●  3V
   A3  ●│         │●  MO
  SDA  ●│         │●  MI
  SCL  ●│         │●  SCK
   TX  ●│         │●  RX
        └─────────┘
```

- **Easy mistakes:** the wing's *A0/A1/A2/SDA* pins are **not** used. QT Py A0/A1/A2/SDA go to wing **6/10/9/5**. SPI is straight through (MO→MO, MI→MI), not crossed like serial TX/RX.
- **Orientation check:** with power off, the wing's GND should beep to the headphone jack shell; powered, wing 3V–GND reads ~3.3 V.
- **Troubleshooting:** `pio run -e probe -t upload -t monitor` runs `src/vs1053_probe.cpp`, which reads the VS1053 registers every second. All 1s = MI/CS/power; all 0s = no power; garbage = MO/SCK. SD failing while the VS1053 works = wing 5 wire or the card (must be FAT32).

```c
#define VS1053_RESET -1   // or 6 if RST is wired to SCL
#define VS1053_CS    18   // A0
#define VS1053_DCS   17   // A1
#define VS1053_DREQ   9   // A2
#define CARDCS        7   // SDA
// SPI.begin(36, 37, 35);  // SCK, MISO, MOSI
```

- **QT Py spare pins:** SCL/6, TX/5, RX/16, A3/8. The Qwiic port's GPIO 41 (SDA1) and 40 (SCL1) are also free.

### General notes (both builds)

- **Power (verified 2026-10-15):** a laptop USB port was **not enough**. Symptoms: the SD card silently reset (all file opens failed until re-init) and playback stopped after a few seconds, worse at `VOLUME` 10 than 20. The VS1053 kept its registers; the SD card is the most sensitive part. A strong 5V supply fixed it. Use a 5V ≥2A adapter with a short cable. Optional hardening: 10–100 µF across the wing's 3V/GND and doubled 3V/GND wires.
- **Never power from two sources at once** (USB from a laptop + adapter on 5V): the rails are joined and one can backfeed the other. Debug on the adapter via the MQTT log topic instead.
- **OTA updates:** the firmware serves a password-protected upload page at `http://192.168.20.116/update` (fixed IP reserved in UniFi; DNS name `halloween-speaker`). `pio run -e ota -t upload` pushes the build with curl, reading the password from 1Password ("OTA – halloween_speaker"). Push, not pull, because the firewall allows Internal → IoT but not the reverse. The Feather's partition table has two 1408 KB app slots, so a failed upload leaves the old firmware running.
- **SD recovery:** before playing, the firmware opens the file; on failure it re-inits the card once and retries, logging the result to `halloween/speaker/log`.
- **Speaker:** wire the 4 Ω / 3 W speaker to one amp channel. Neither terminal goes to ground.
- **Enclosure (decided 2026-10-15):** the whole device lives outdoors under the covered porch in an Adafruit #905 large weatherproof box. With the stacked Feather the electronics are one ~25 mm tall block, so the small #903 (30 mm usable height) may now fit; the large box still leaves room for the speaker, gland and fingers.
  - **Speaker:** mount the #4445 against one wall with two M3 screws (sealed with silicone or nylon washers). Drill a grille of ~3 mm holes over the cone and glue acoustic mesh behind it. Face that wall down or sideways toward visitors, never up.
  - **Power entry:** USB cable through a PG-7 cable gland with a drip loop outside. The 5V ≥2A adapter must be indoors or in a weatherproof outlet.
  - **Boards:** nylon M2.5 standoffs on a plastic plate fixed to the M4 bosses; no metal near the boards.
  - **Moisture:** a silica gel pack inside and a small drip hole at the lowest corner. Condensation on cold nights is the main outdoor risk.
  - **Wi-Fi:** the plastic box barely attenuates; check the RSSI sensor at the door after installation.

## Implementation phases

### Phase 1: Hardware validation

Done when an MP3 plays through the existing speaker.

- [x] Identify the exact FeatherWing revision (#3436, amplified)
- [x] Select the ESP32 board (Feather ESP32-S3 #5477, stacked; QT Py ESP32-S3 used through 2026-10-19)
- [x] QT Py ↔ FeatherWing wiring *(verified 2026-10-15: VS1053 OK, SD OK)*
- [ ] Feather ESP32-S3 stacked with the wing: `VS1053 OK`, `SD OK`, playback
- [ ] Verify RST behavior on the bench
- [ ] Power-cycle test with the wing and SD attached
- [x] Verify power requirements *(2026-10-15: laptop USB insufficient; 5V ≥2A adapter works)*
- [x] Connect one speaker and play from SD *(2026-10-15, via MQTT)*

### Phase 2: MQTT integration

Done when publishing an MQTT message triggers a sound.

- [x] Connect Wi-Fi and the MQTT broker *(verified 2026-10-15)*
- [x] Implement the play command and status topics *(implemented; playback untested until the wing is wired)*
- [x] Add random selection, cooldown, and playback-state handling *(implemented; playback untested until the wing is wired)*
- [x] Home Assistant discovery *(verified 2026-10-15)*
- [x] Network access: UniFi rule (IoT MAC 98:c3:77:60:73:18, the Feather; was cc:ba:97:1d:09:00 → svc-01 TCP 1883) and Mosquitto user `halloween_speaker` with ACL *(verified 2026-10-15)*

### Phase 3: Home Assistant

Done when one doorbell press triggers exactly one sound.

- [ ] Identify the AD410 physical button-press event
- [ ] Create the automation and the enable/disable toggle
- [ ] Test the full trigger-to-playback path

### Phase 4: Deployment

- [ ] Install the electronics in the enclosure and position the speaker
- [ ] Test volume at visitor distance and check Wi-Fi at the door
- [ ] Test repeated presses
- [ ] Verify recovery after an ESP32 reboot and an MQTT reconnect

## Design priorities

In descending order:

1. **Reliability:** every valid press produces exactly one sound.
2. **Low latency:** playback starts promptly after the press.
3. **Local operation:** no cloud dependency.
4. **Hardware reuse:** buy as little as possible.
5. **Simplicity:** no unnecessary abstraction or infrastructure.
6. **Maintainability:** keep configuration separate from logic.

**Out of scope:** audio streaming, voice assistants, microphones, and unrelated ESPHome features.

## Open decisions

Resolve these before implementation:

- [x] Which ESP32 board? **Feather ESP32-S3 #5477** (2026-10-20; was QT Py ESP32-S3 N4R2)
- [x] What is the exact FeatherWing revision? **#3436, amplified**
- [ ] Is the 4 Ω / 3 W speaker loud enough at the door, and what is the maximum clean volume setting?
- [ ] Which entity or event does the AD410 emit for a physical press?
- [ ] Should a press during playback interrupt it or be ignored? (The current spec says ignore.)
- [x] Will the electronics sit indoors with speaker cable run outside, or be fully outdoors? **Fully outdoors, Adafruit #905 box** (2026-10-15)

## Implementation instructions

**First task:** produce a verified wiring diagram from the QT Py ESP32-S3 for the amplified Music Maker FeatherWing.

- Build incrementally, starting with hardware checks and a minimal playback test.
- Treat current Adafruit documentation and library examples as authoritative for the VS1053, the FeatherWing pinout, and ESP32 compatibility.
- Prefer maintained libraries over custom drivers.
- Use PlatformIO with the Arduino framework unless a technical limit forces otherwise.
- Never invent pin assignments or assume SPI wiring is the same across ESP32 boards.
