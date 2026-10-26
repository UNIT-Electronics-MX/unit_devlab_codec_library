// Audio output test with ESP32-WROOM or ESP32-C6 and the ES8311 codec.
#include <DevLab_Codec_ES.h>
#include <DevLab_AudioTools.h>
#include "AudioTools/AudioLibs/I2SCodecStream.h"

#if defined(CONFIG_IDF_TARGET_ESP32C6)
// ESP32-C6 pinout used by the DualMCU MP3 encoder example.
#define SDAPIN 22
#define SCLPIN 23
#define MCLKPIN 21
#define BCLKPIN 3
#define WSPIN 15
#define DOPIN 20  // ESP32-C6 output -> ES8311 DIN
#elif defined(CONFIG_IDF_TARGET_ESP32) || defined(CONFIG_IDF_TARGET_ESP32S3)
// ESP32-WROOM pinout; keep the existing mapping for identically wired S3 boards.
#define SDAPIN 21
#define SCLPIN 22
#define MCLKPIN 0
#define BCLKPIN 4
#define WSPIN 16
#define DOPIN 15  // ESP32 output -> ES8311 DIN
#else
#error "This example supports ESP32-WROOM and ESP32-C6"
#endif

#define I2CSPEED 100000
#define ES8311_ADDR 0x18
#define DIPIN -1  // I2S input is unused

AudioInfo audio_info(44100, 2, 16);
SineWaveGenerator<int16_t> sine_wave(14000);
GeneratedSoundStream<int16_t> sound_stream(sine_wave);
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s_out_stream(audio_board);
StreamCopy copier(i2s_out_stream, sound_stream);
TwoWire myWire(0);

bool audio_ready = false;

void checkCodecI2C() {
  myWire.beginTransmission(ES8311_ADDR);
  const byte result = myWire.endTransmission();
  Serial.printf("I2C ES8311 (0x%02X): %s (code %u)\n",
                ES8311_ADDR, result == 0 ? "OK" : "no response", result);

  if (result != 0) {
    Serial.print("Other detected I2C addresses:");
    bool found = false;
    for (byte address = 1; address < 127; ++address) {
      myWire.beginTransmission(address);
      if (myWire.endTransmission() == 0) {
        Serial.printf(" 0x%02X", address);
        found = true;
      }
    }
    if (!found) Serial.print(" none");
    Serial.println();
  }
}

int readCodecRegister(byte address) {
  myWire.beginTransmission(ES8311_ADDR);
  myWire.write(address);
  if (myWire.endTransmission(false) != 0) return -1;
  if (myWire.requestFrom((uint8_t)ES8311_ADDR, (uint8_t)1) != 1) return -1;
  return myWire.read();
}

void printCodecState() {
  const byte addresses[] = { 0x09, 0x12, 0x31, 0x32, 0xFD, 0xFE };
  Serial.print("ES8311 registers:");
  for (byte address : addresses) {
    const int value = readCodecRegister(address);
    if (value < 0) {
      Serial.printf(" %02X=ERROR", address);
    } else {
      Serial.printf(" %02X=%02X", address, value);
    }
  }
  Serial.println();
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);
  delay(2000);

  Serial.println("Starting ES8311 test...");
  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: invalid ES8311 pins or pins already registered");
    return;
  }

  auto config = i2s_out_stream.defaultConfig(TX_MODE);
  config.copyFrom(audio_info);
  config.is_master = true;
  config.sd_active = false;

  // I2SCodecStream initializes the pins, codec, and I2S bus.
  const bool stream_ok = i2s_out_stream.begin(config);
  Serial.printf("Codec and I2S startup: %s\n", stream_ok ? "OK" : "ERROR");
  checkCodecI2C();
  if (!stream_ok) return;

  const bool volume_ok = i2s_out_stream.setVolume(0.7f);
  const bool unmute_ok = i2s_out_stream.setMute(false);
  Serial.printf("Volume 70%%: %s, unmute: %s\n",
                volume_ok ? "OK" : "ERROR", unmute_ok ? "OK" : "ERROR");
  printCodecState();

  if (!sound_stream.begin(audio_info)) {
    Serial.println("Sine wave generator: ERROR");
    return;
  }
  sine_wave.setFrequency(N_B4);  // 493.88 Hz
  audio_ready = true;
  Serial.println("Sine wave generator: OK");
}

void loop() {
  if (!audio_ready) {
    delay(1000);
    return;
  }

  static size_t bytes_sent = 0;
  static unsigned long last_report = millis();
  bytes_sent += copier.copy();
  if (millis() - last_report >= 1000) {
    Serial.printf("Data sent to I2S: %u bytes/s\n", (unsigned int)bytes_sent);
    bytes_sent = 0;
    last_report = millis();
  }
}
