// Halloween speaker: Phase 2 - MQTT integration with Home Assistant discovery
#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_VS1053.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Wire.h>
#include <Adafruit_MAX1704X.h>
#include <WebServer.h>
#include <HTTPUpdateServer.h>
#include <vector>
#include <algorithm>
#include "config.h"
#include "pins.h"

#define VOLUME       20  // boot attenuation until HA's retained volume arrives; lower = louder
// Volume slider 0-100% maps onto VS1053 attenuation (0.5 dB steps); 0% mutes
#define VOLUME_LOUDEST  0  // attenuation at 100%; tested on a strong supply
#define VOLUME_QUIETEST 85 // attenuation at 1% (-42.5 dB)
#define VOLUME_MUTE   254

// Playlist: every .mp3 in the SD card root, scanned at boot
static std::vector<String> playlist;

// Device topics and identifiers
static const char* AVAILABILITY_TOPIC = "halloween/speaker/availability";
static const char* PLAY_TOPIC = "halloween/speaker/play";
static const char* RESTART_TOPIC = "halloween/speaker/restart";
static const char* STATUS_TOPIC = "halloween/speaker/status";
static const char* LOG_TOPIC = "halloween/speaker/log";
static const char* VOLUME_SET_TOPIC = "halloween/speaker/volume/set";
static const char* VOLUME_STATE_TOPIC = "halloween/speaker/volume";
static const char* MODE_SET_TOPIC = "halloween/speaker/mode/set";
static const char* MODE_STATE_TOPIC = "halloween/speaker/mode";
static const char* RSSI_TOPIC = "halloween/speaker/rssi";
static const char* BATTERY_VOLTAGE_TOPIC = "halloween/speaker/battery/voltage";
static const char* BATTERY_PERCENT_TOPIC = "halloween/speaker/battery/percent";
static const char* HA_STATUS_TOPIC = "homeassistant/status";

// MQTT discovery topics
static const char* DISCOVERY_BASE = "homeassistant";

// Globals
Adafruit_VS1053_FilePlayer player(VS1053_RESET, VS1053_CS, VS1053_DCS, VS1053_DREQ, CARDCS);
WiFiClient wifiClient;
WebServer otaServer(80);
HTTPUpdateServer otaUpdater;
PubSubClient client(wifiClient);
Adafruit_MAX17048 fuelGauge;
static bool fuelGaugeReady = false;

// Playback state
enum PlaybackState { STATE_IDLE = 0, STATE_PLAYING = 1, STATE_ERROR = 2 };
static PlaybackState playbackState = STATE_IDLE;
static unsigned long playbackStartTime = 0;
static unsigned long lastPlayTime = 0;
static int lastPlayedIndex = -1;
static int sequenceIndex = -1;          // last track played by "next"; RAM only, so a reboot restarts at the top
static bool sequentialMode = false;     // what a play command without "sound" does
static bool audioReady = false;  // false if VS1053 or SD failed at boot; stay online and report error

// Connection timing
static unsigned long lastMqttAttempt = 0;
static unsigned long lastRssiPublish = 0;

// Forward declarations
void onMqttMessage(char* topic, byte* payload, unsigned int length);
void reconnectMqtt();
void updatePlaybackState(PlaybackState newState);
void handlePlayCommand(const char* payload);
void publishDiscovery();
void publishStatus();
void publishAvailability(bool online);

// Log to serial and, when connected, to halloween/speaker/log (debug without a USB cable)
void logf(const char* fmt, ...) {
  char buf[256];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.print(buf);
  if (client.connected()) {
    size_t n = strlen(buf);
    if (n && buf[n - 1] == '\n') buf[n - 1] = '\0';
    client.publish(LOG_TOPIC, buf);
  }
}

uint8_t volumeToAttenuation(int pct) {
  if (pct <= 0) return VOLUME_MUTE;
  return VOLUME_QUIETEST - (pct - 1) * (VOLUME_QUIETEST - VOLUME_LOUDEST) / 99;
}

void applyVolume(int pct) {
  pct = constrain(pct, 0, 100);
  uint8_t att = volumeToAttenuation(pct);
  if (audioReady) player.setVolume(att, att);
  client.publish(VOLUME_STATE_TOPIC, String(pct).c_str(), true);
  logf("Volume %d%% (attenuation %u)\n", pct, att);
}

void applyMode(const char* mode) {
  if (strcmp(mode, "sequential") != 0 && strcmp(mode, "random") != 0) {
    logf("Ignoring mode: %s\n", mode);
    return;
  }
  sequentialMode = strcmp(mode, "sequential") == 0;
  client.publish(MODE_STATE_TOPIC, mode, true);
  logf("Mode %s\n", mode);
}

void scanPlaylist() {
  File root = SD.open("/");
  for (File f = root.openNextFile(); f; f = root.openNextFile()) {
    String name = f.name();
    if (name.startsWith("/")) name = name.substring(1);
    String lower = name;
    lower.toLowerCase();
    // Skip folders and macOS "._name.mp3" metadata files, which are not audio
    if (!f.isDirectory() && !name.startsWith(".") && lower.endsWith(".mp3")) playlist.push_back("/" + name);
    f.close();
  }
  root.close();
  std::sort(playlist.begin(), playlist.end());
  Serial.printf("Found %u MP3 file(s) on SD:\n", (unsigned)playlist.size());
  for (const String& path : playlist) Serial.printf("  %s\n", path.c_str());
  if (playlist.empty()) Serial.println("ERROR: no .mp3 files in the SD card root");
}

void setup() {
  Serial.begin(115200);
  // Native USB: the port re-enumerates on reset; wait up to 3 s so early messages aren't lost
  while (!Serial && millis() < 3000) delay(10);
  Serial.println("\nHalloween speaker: Phase 2 - MQTT integration");

  // The fuel gauge sits on the I2C bus, which is unpowered until PIN_I2C_POWER goes high
  pinMode(PIN_I2C_POWER, OUTPUT);
  digitalWrite(PIN_I2C_POWER, HIGH);
  delay(10);
  Wire.begin();
  fuelGaugeReady = fuelGauge.begin();
  Serial.printf("MAX17048 fuel gauge %s\n", fuelGaugeReady ? "found" : "NOT found");

  // Initialize SPI and VS1053
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  if (!player.begin()) {
    Serial.println("ERROR: VS1053 not found - check SPI, CS, DCS, DREQ wiring");
  } else {
    Serial.println("VS1053 OK");
    player.setVolume(VOLUME, VOLUME);

    // Initialize SD card
    if (!SD.begin(CARDCS)) {
      Serial.println("ERROR: SD init failed - check card (FAT32) and SDCS wiring");
    } else {
      Serial.println("SD OK");
      scanPlaylist();
      audioReady = !playlist.empty();
    }
  }
  if (!audioReady) playbackState = STATE_ERROR;

  // Set up MQTT client
  client.setServer(MQTT_HOST, MQTT_PORT);
  client.setCallback(onMqttMessage);
  client.setBufferSize(1024);

  // Connect to Wi-Fi (blocking, but only once at startup)
  Serial.printf("Connecting to Wi-Fi: %s\n", WIFI_SSID);
  WiFi.setHostname("halloween-speaker");
  WiFi.mode(WIFI_STA);
  Serial.printf("Wi-Fi MAC: %s\n", WiFi.macAddress().c_str());
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("\nWi-Fi connected: %s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("\nERROR: Wi-Fi connection failed - will retry in loop");
  }

  // Firmware upload page; Internal -> IoT is allowed by the firewall, the reverse is not, so push beats pull
  otaUpdater.setup(&otaServer, "/update", OTA_USER, OTA_PASS);
  otaServer.begin();

  Serial.println("Setup complete");
}

void loop() {
  // Non-blocking MQTT reconnect (every 5 seconds if disconnected)
  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttAttempt >= 5000) {
      lastMqttAttempt = now;
      reconnectMqtt();
    }
  } else {
    client.loop();
  }

  // Non-blocking playback: feed the VS1053 buffer
  if (playbackState == STATE_PLAYING) {
    if (!player.playingMusic) {
      // Playback finished
      logf("Playback finished after %lu ms\n", millis() - lastPlayTime);
      updatePlaybackState(STATE_IDLE);
      publishStatus();
    } else {
      // Feed the buffer continuously
      player.feedBuffer();
    }
  }

  // Publish RSSI every 60 seconds
  unsigned long now = millis();
  if (client.connected() && (now - lastRssiPublish >= 60000)) {
    lastRssiPublish = now;
    int rssi = WiFi.RSSI();
    char rssiStr[16];
    snprintf(rssiStr, sizeof(rssiStr), "%d", rssi);
    client.publish(RSSI_TOPIC, rssiStr, true);
    Serial.printf("RSSI: %d dBm\n", rssi);

    if (fuelGaugeReady) {
      float volts = fuelGauge.cellVoltage();
      float pct = min(fuelGauge.cellPercent(), 100.0f);  // reads slightly over 100 while charging
      client.publish(BATTERY_VOLTAGE_TOPIC, String(volts, 2).c_str(), true);
      client.publish(BATTERY_PERCENT_TOPIC, String(pct, 0).c_str(), true);
      Serial.printf("Battery: %.2f V, %.0f%%\n", volts, pct);
    }
  }

  otaServer.handleClient();

  delay(10);  // Small delay to prevent watchdog timeout
}

void reconnectMqtt() {
  Serial.print("Attempting MQTT connection to ");
  Serial.print(MQTT_HOST);
  Serial.print(":");
  Serial.println(MQTT_PORT);

  // Fixed client ID: on reconnect the broker drops the stale session (and fires its "offline" will)
  // immediately, before our "online"; a random ID let the old will arrive later and win
  String clientId = DEVICE_ID;

  // Attempt connection with last-will message
  if (client.connect(
        clientId.c_str(),
        MQTT_USER,
        MQTT_PASS,
        AVAILABILITY_TOPIC,     // Will topic
        1,                       // Will QoS (retained)
        true,                    // Will retain
        "offline"                // Will message
      )) {
    Serial.println("MQTT connected");

    // Publish online and discovery
    publishAvailability(true);
    publishDiscovery();
    publishStatus();

    // Subscribe to play and HA status topics
    client.subscribe(PLAY_TOPIC);
    client.subscribe(RESTART_TOPIC);
    client.subscribe(HA_STATUS_TOPIC);
    client.subscribe(VOLUME_SET_TOPIC);  // HA retains it, so the broker replays the last volume on connect
    client.subscribe(MODE_SET_TOPIC);    // same for mode
    Serial.printf("Subscribed to %s, %s, %s, %s and %s\n", PLAY_TOPIC, RESTART_TOPIC, HA_STATUS_TOPIC, VOLUME_SET_TOPIC, MODE_SET_TOPIC);
  } else {
    Serial.print("MQTT connection failed, rc=");
    Serial.println(client.state());
  }
}

void onMqttMessage(char* topic, byte* payload, unsigned int length) {
  Serial.printf("MQTT received on %s: ", topic);
  for (unsigned int i = 0; i < length; i++) Serial.print((char)payload[i]);
  Serial.println();

  // Check for HA status republish trigger
  if (strcmp(topic, HA_STATUS_TOPIC) == 0) {
    // Home Assistant came online: republish discovery and current state
    if (length > 0 && payload[0] == 'o') {  // Check if it starts with 'o' for "online"
      Serial.println("Home Assistant online detected - republishing discovery");
      publishDiscovery();
      publishAvailability(true);
      publishStatus();
    }
    return;
  }

  if (strcmp(topic, VOLUME_SET_TOPIC) == 0) {
    char buf[8] = {0};
    memcpy(buf, payload, min(length, (unsigned int)sizeof(buf) - 1));
    char* end;
    long pct = strtol(buf, &end, 10);
    if (end == buf) {
      logf("Ignoring volume: not a number\n");
      return;
    }
    applyVolume((int)pct);
    return;
  }

  if (strcmp(topic, MODE_SET_TOPIC) == 0) {
    char buf[16] = {0};
    memcpy(buf, payload, min(length, (unsigned int)sizeof(buf) - 1));
    applyMode(buf);
    return;
  }

  if (strcmp(topic, RESTART_TOPIC) == 0) {
    if (length == 7 && memcmp(payload, "restart", 7) == 0) {
      publishAvailability(false);
      delay(100);
      ESP.restart();
    }
    return;
  }

  // Parse play command
  if (strcmp(topic, PLAY_TOPIC) == 0) {
    // Convert payload to null-terminated string for JSON parsing
    String payloadStr;
    for (unsigned int i = 0; i < length; i++) {
      payloadStr += (char)payload[i];
    }
    handlePlayCommand(payloadStr.c_str());
  }
}

void handlePlayCommand(const char* payload) {
  if (!audioReady) {
    Serial.println("Ignoring play command: audio hardware not initialized");
    return;
  }

  // Check cooldown
  unsigned long now = millis();
  if (now - lastPlayTime < COOLDOWN_MS && playbackState == STATE_IDLE) {
    Serial.println("Ignoring play command: in cooldown");
    return;
  }

  // Check if already playing
  if (playbackState == STATE_PLAYING) {
    Serial.println("Ignoring play command: already playing");
    return;
  }

  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, payload);

  if (error) {
    Serial.printf("JSON parse error: %s\n", error.c_str());
    updatePlaybackState(STATE_ERROR);
    publishStatus();
    return;
  }

  // Get sound choice; none means "follow the Mode setting"
  const char* sound = doc["sound"] | (sequentialMode ? "next" : "random");

  // Determine file to play
  const char* fileToPlay = nullptr;
  const int count = (int)playlist.size();

  if (strcmp(sound, "random") == 0) {
    // Pick a random file, avoiding the last one if possible
    int nextIndex;
    do {
      nextIndex = random(count);
    } while (nextIndex == lastPlayedIndex && count > 1);

    fileToPlay = playlist[nextIndex].c_str();
    lastPlayedIndex = nextIndex;
    Serial.printf("Selected random: %s\n", fileToPlay);
  } else if (strcmp(sound, "next") == 0) {
    sequenceIndex = (sequenceIndex + 1) % count;  // wraps to the first track after the last
    fileToPlay = playlist[sequenceIndex].c_str();
    lastPlayedIndex = sequenceIndex;
    logf("Selected next: %s (%d of %d)\n", fileToPlay, sequenceIndex + 1, count);
  } else {
    // Specific file: only names found by the boot scan are playable, which also rules out path tricks
    String wanted = sound[0] == '/' ? String(sound) : "/" + String(sound);
    auto it = std::find(playlist.begin(), playlist.end(), wanted);
    if (it == playlist.end()) {
      Serial.printf("ERROR: File not found: %s\n", wanted.c_str());
      updatePlaybackState(STATE_ERROR);
      publishStatus();
      return;
    }
    fileToPlay = it->c_str();
    Serial.printf("Playing specific file: %s\n", fileToPlay);
  }

  // An SD card can reset on a supply dip while the library still thinks it's mounted; re-init once and retry
  File check = SD.open(fileToPlay);
  if (!check) {
    SD.end();
    bool remounted = SD.begin(CARDCS);
    check = SD.open(fileToPlay);
    logf("SD open failed; re-init %s, retry %s\n", remounted ? "ok" : "FAILED", check ? "ok" : "FAILED");
  }
  if (check) check.close();

  // Start playback
  if (!player.startPlayingFile(fileToPlay)) {
    logf("ERROR: Failed to start playback: %s\n", fileToPlay);
    updatePlaybackState(STATE_ERROR);
    publishStatus();
    return;
  }

  logf("Playback started: %s\n", fileToPlay);
  lastPlayTime = now;
  updatePlaybackState(STATE_PLAYING);
  publishStatus();
}

void updatePlaybackState(PlaybackState newState) {
  playbackState = newState;
  if (newState == STATE_PLAYING) {
    playbackStartTime = millis();
  }
}

void publishStatus() {
  const char* statusStr;
  switch (playbackState) {
    case STATE_PLAYING:
      statusStr = "playing";
      break;
    case STATE_ERROR:
      statusStr = "error";
      break;
    case STATE_IDLE:
    default:
      statusStr = "idle";
      break;
  }

  if (client.connected()) {
    client.publish(STATUS_TOPIC, statusStr, true);
    Serial.printf("Published status: %s\n", statusStr);
  }
}

void publishAvailability(bool online) {
  if (client.connected()) {
    const char* status = online ? "online" : "offline";
    client.publish(AVAILABILITY_TOPIC, status, true);
    Serial.printf("Published availability: %s\n", status);
  }
}

void publishDiscovery() {
  if (!client.connected()) return;

  String deviceId = String(DEVICE_ID);

  // Base device object (shared by all entities)
  JsonDocument baseDoc;
  baseDoc["identifiers"][0] = deviceId;
  baseDoc["name"] = "Halloween Speaker";
  baseDoc["manufacturer"] = "Adafruit";
  baseDoc["model"] = "Feather ESP32-S3 + Music Maker FeatherWing";

  String deviceJson;
  serializeJson(baseDoc, deviceJson);

  // 1. Status sensor
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/sensor/" + deviceId + "_status/config";
    JsonDocument doc;
    doc["name"] = "Status";
    doc["unique_id"] = deviceId + "_status";
    doc["state_topic"] = STATUS_TOPIC;
    doc["icon"] = "mdi:ghost";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  // 2. Play button
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/button/" + deviceId + "_play/config";
    JsonDocument doc;
    doc["name"] = "Play";
    doc["unique_id"] = deviceId + "_play";
    doc["command_topic"] = PLAY_TOPIC;
    doc["payload_press"] = "{\"action\":\"play\"}";  // no "sound": the device follows its Mode setting
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  // Restart button; the command is never retained so reconnects cannot reboot the device.
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/button/" + deviceId + "_restart/config";
    JsonDocument doc;
    doc["name"] = "Restart";
    doc["unique_id"] = deviceId + "_restart";
    doc["command_topic"] = RESTART_TOPIC;
    doc["payload_press"] = "restart";
    doc["retain"] = false;
    doc["device_class"] = "restart";
    doc["entity_category"] = "config";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  // 3. RSSI sensor
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/sensor/" + deviceId + "_rssi/config";
    JsonDocument doc;
    doc["name"] = "Wi-Fi Signal";
    doc["unique_id"] = deviceId + "_rssi";
    doc["state_topic"] = RSSI_TOPIC;
    doc["device_class"] = "signal_strength";
    doc["unit_of_measurement"] = "dBm";
    doc["entity_category"] = "diagnostic";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  // 3b. Battery sensors
  {
    struct { const char* key; const char* name; const char* topic; const char* deviceClass; const char* unit; } sensors[] = {
      {"battery_voltage", "Battery Voltage", BATTERY_VOLTAGE_TOPIC, "voltage", "V"},
      {"battery", "Battery", BATTERY_PERCENT_TOPIC, "battery", "%"},
    };
    for (auto& s : sensors) {
      String discoveryTopic = String(DISCOVERY_BASE) + "/sensor/" + deviceId + "_" + s.key + "/config";
      JsonDocument doc;
      doc["name"] = s.name;
      doc["unique_id"] = deviceId + "_" + s.key;
      doc["state_topic"] = s.topic;
      doc["device_class"] = s.deviceClass;
      doc["unit_of_measurement"] = s.unit;
      doc["state_class"] = "measurement";
      doc["entity_category"] = "diagnostic";
      doc["availability_topic"] = AVAILABILITY_TOPIC;
      doc["availability_mode"] = "latest";
      deserializeJson(baseDoc, deviceJson);
      doc["device"] = baseDoc.as<JsonObject>();

      String payload;
      serializeJson(doc, payload);
      bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
      Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
    }
  }

  // 4. Volume slider
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/number/" + deviceId + "_volume/config";
    JsonDocument doc;
    doc["name"] = "Volume";
    doc["unique_id"] = deviceId + "_volume";
    doc["command_topic"] = VOLUME_SET_TOPIC;
    doc["state_topic"] = VOLUME_STATE_TOPIC;
    doc["retain"] = true;  // HA retains commands so the device gets its volume back after a reboot
    doc["min"] = 0;
    doc["max"] = 100;
    doc["step"] = 1;
    doc["mode"] = "slider";
    doc["unit_of_measurement"] = "%";
    doc["icon"] = "mdi:volume-high";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  // 5. Mode select
  {
    String discoveryTopic = String(DISCOVERY_BASE) + "/select/" + deviceId + "_mode/config";
    JsonDocument doc;
    doc["name"] = "Mode";
    doc["unique_id"] = deviceId + "_mode";
    doc["command_topic"] = MODE_SET_TOPIC;
    doc["state_topic"] = MODE_STATE_TOPIC;
    doc["retain"] = true;  // survives reboots, like volume
    doc["options"][0] = "random";
    doc["options"][1] = "sequential";
    doc["icon"] = "mdi:playlist-play";
    doc["availability_topic"] = AVAILABILITY_TOPIC;
    doc["availability_mode"] = "latest";
    deserializeJson(baseDoc, deviceJson);
    doc["device"] = baseDoc.as<JsonObject>();

    String payload;
    serializeJson(doc, payload);
    bool published = client.publish(discoveryTopic.c_str(), payload.c_str(), true);
    Serial.printf("Discovery %s: %s\n", published ? "published" : "FAILED", discoveryTopic.c_str());
  }

  Serial.println("Home Assistant discovery published");
}
