#include <UnitDevlabCodecPlayback.h>
#include <SD.h>
#include <DevLab_AudioSourceSD.h>

// SD card and file format
const char* startFilePath = "/";
const char* ext = ".mp3";
const int sdChipSelect = SS;
AudioSourceSD source(startFilePath, ext, sdChipSelect);

// Decoder
MP3DecoderHelix decoder;

// I2C and I2S pins
#define SDAPIN     21
#define SCLPIN     22
#define ES8311_ADDR 0x18
#define I2CSPEED   100000

#define MCLKPIN 0
#define BCLKPIN 4
#define WSPIN 16
#define DOPIN 15   // ESP32 output -> ES8311 DIN
#define DIPIN      -1   // Input is unused

// Codec objects
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);  // Uses the ES8311 codec

// Player
AudioPlayer player(source, i2s, decoder);

// I2C
TwoWire myWire = TwoWire(0);

// State
float currentVolume = 0.1;
float lastVolume = 0.1;
bool isMuted = false;
bool isPaused = false;
bool playerReady = false;

void listFiles(File directory, int depth = 0) {
  File entry;
  while ((entry = directory.openNextFile())) {
    for (int i = 0; i < depth; ++i) Serial.print("  ");
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      if (depth < 8) listFiles(entry, depth + 1);
    } else {
      Serial.printf(" (%u bytes)\n", (unsigned int)entry.size());
    }
    entry.close();
  }
}

// Metadata logging
void printMetaData(MetaDataType type, const char* str, int len) {
  Serial.print("==> ");
  Serial.print(toStr(type));
  Serial.print(": ");
  Serial.println(str);
}

void printHelp() {
  Serial.println("Available commands:");
  Serial.println("  v0.1 - v1.0  - Change volume (10% - 100%)");
  Serial.println("  p           - Pause playback");
  Serial.println("  r           - Resume playback");
  Serial.println("  n           - Next track");
  Serial.println("  b           - Previous track");
  Serial.println("  m           - Mute");
  Serial.println("  u           - Unmute");
  Serial.println("  ?           - Show help");
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Info);
  delay(1000);
  Serial.println("Initializing...");

  Serial.printf("Mounting microSD with CS=%d...\n", sdChipSelect);
  if (!SD.begin(sdChipSelect) || SD.cardType() == CARD_NONE) {
    Serial.println("ERROR: could not mount microSD. Check CS, SPI, and power.");
    return;
  }
  Serial.printf("MicroSD mounted: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  File root = SD.open(startFilePath);
  if (!root || !root.isDirectory()) {
    Serial.println("ERROR: could not open the microSD root directory.");
    return;
  }
  Serial.println("Files on microSD:");
  listFiles(root);
  root.close();

  source.begin();
  const long mp3Count = source.size();
  Serial.printf(".mp3 files found: %ld\n", mp3Count);
  if (mp3Count == 0) {
    Serial.println("ERROR: no .mp3 files found from the microSD root.");
    return;
  }

  // Configure codec I2C and I2S pins
  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: invalid ES8311 pins or pins already registered");
    return;
  }
  // I2SCodecStream initializes the pins and ES8311 codec.
  auto cfg = i2s.defaultConfig(TX_MODE);
  if (!i2s.begin(cfg)) {
    Serial.println("ERROR: could not start ES8311/I2S");
    return;
  }

  // Configure and start the player
  player.setMetadataCallback(printMetaData);
  playerReady = player.begin();
  if (!playerReady) {
    Serial.println("ERROR: could not open the first MP3.");
    return;
  }
  player.setVolume(currentVolume); // Initial volume at 10%

  Serial.println("Playback ready.");
  printHelp();
}

void loop() {
  if (playerReady && !isPaused) player.copy();

  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("v")) {
      float v = input.substring(1).toFloat();
      if (v >= 0.0 && v <= 1.0) {
        currentVolume = v;
        isMuted = false;
        player.setVolume(currentVolume);
        Serial.print("Volume: ");
        Serial.println(currentVolume);
      } else {
        Serial.println("Invalid value. Use v0.0 to v1.0");
      }

    } else if (input == "p") {
      isPaused = true;
      Serial.println("Playback paused");

    } else if (input == "r") {
      isPaused = false;
      Serial.println("Playback resumed");

    } else if (input == "n") {
      player.next();
      Serial.println("Next track");

    } else if (input == "b") {
      player.previous();
      Serial.println("Previous track");

    } else if (input == "m") {
      if (!isMuted) {
        lastVolume = currentVolume;
        player.setVolume(0.0);
        isMuted = true;
        Serial.println("Muted");
      }

    } else if (input == "u") {
      if (isMuted) {
        player.setVolume(lastVolume);
        isMuted = false;
        Serial.println("Unmuted");
      }

    } else if (input == "?") {
      printHelp();

    } else {
      Serial.println("Unknown command. Use '?' for help.");
    }
  }
}
