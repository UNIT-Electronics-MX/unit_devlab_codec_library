#include <UnitDevlabCodecPlayback.h>
#include <DevLab_AudioSourceSD.h>

// SD card and file format
const char* startFilePath = "/";
const char* ext = "mp3";
AudioSourceSD source(startFilePath, ext);

// Decoder
MP3DecoderHelix decoder;

// I2C and I2S pins
#define SDAPIN     21
#define SCLPIN     22
#define ES8311_ADDR 0x18
#define I2CSPEED   100000

#define MCLKPIN    0   // Use -1 if MCLK is not connected
#define BCLKPIN    17
#define WSPIN      16
#define DOPIN      4    // ESP32 data output -> ES8311 DIN
#define DIPIN      -1   // Input is unused

// Codec objects
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);  // Uses the ES8311 codec

// Player
AudioPlayer player(source, i2s, decoder);

// I2C
TwoWire myWire = TwoWire(0);

// Current volume (0.0 to 1.0)
float currentVolume = 0.02;

// Optional metadata logging
void printMetaData(MetaDataType type, const char* str, int len) {
  Serial.print("==> ");
  Serial.print(toStr(type));
  Serial.print(": ");
  Serial.println(str);
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Info);
  delay(1000);
  Serial.println("Initializing...");

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
  player.begin();
  player.setVolume(currentVolume); // initial volume at 10%

  Serial.println("Ready to play MP3 from microSD through ES8311");
  Serial.println("Enter 'v0.5' to set the volume to 50%");
}

void loop() {
  // Continuous playback
  player.copy();

  // Read serial commands
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    // Change volume: for example, v0.3
    if (input.startsWith("v")) {
      float v = input.substring(1).toFloat();
      if (v >= 0.0 && v <= 1.0) {
        currentVolume = v;
        player.setVolume(currentVolume);
        Serial.print("Volume updated to: ");
        Serial.println(currentVolume);
      } else {
        Serial.println("Invalid value. Use v0.0 to v1.0 (e.g. v0.3)");
      }
    }
  }
}
