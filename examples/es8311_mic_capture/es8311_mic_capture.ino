// Capture ES8311 microphone samples on ESP32-WROOM or ESP32-C6.
// PCM samples can be passed to a network, storage, or DSP transport.
#include <DevLab_ES8311Microphone.h>

#if defined(CONFIG_IDF_TARGET_ESP32C6)
constexpr int kSda = 22;
constexpr int kScl = 23;
constexpr int kMclk = 21;
constexpr int kBclk = 3;
constexpr int kWs = 15;
constexpr int kCodecDin = 20;
#elif defined(CONFIG_IDF_TARGET_ESP32)
constexpr int kSda = 21;
constexpr int kScl = 22;
constexpr int kMclk = 0;
constexpr int kBclk = 4;
constexpr int kWs = 16;
constexpr int kCodecDin = 15;
#else
#error "This example supports ESP32-WROOM and ESP32-C6"
#endif

constexpr int kCodecDout = 5;  // ES8311 ADC DOUT -> ESP32 I2S input.
constexpr int kSampleRate = 16000;
constexpr float kCodecInputGain = 1.0f;  // Reduce if loud input clips.
constexpr size_t kSamplesPerBlock = 256;

TwoWire codecWire(0);
unit_devlab::ES8311Microphone microphone(codecWire);
int16_t pcm[kSamplesPerBlock];
uint32_t capturedFrames = 0;
uint16_t peak = 0;
uint32_t lastReportAt = 0;

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);

  const unit_devlab::ES8311Pinout pins{
      kSda, kScl, kMclk, kBclk, kWs, kCodecDin, kCodecDout, 0x18, 100000};
  if (!microphone.begin(pins, kSampleRate, kCodecInputGain)) {
    Serial.println("ES8311 microphone startup failed");
    return;
  }
  lastReportAt = millis();
  Serial.printf("ES8311 microphone ready: %d Hz, 16-bit mono PCM, GPIO%d data input\n",
                kSampleRate, kCodecDout);
}

void loop() {
  if (!microphone.isActive()) {
    delay(1000);
    return;
  }

  const size_t frames = microphone.readMono(pcm, kSamplesPerBlock);
  capturedFrames += frames;
  for (size_t i = 0; i < frames; ++i) {
    const int32_t sample = pcm[i];
    const uint16_t level = static_cast<uint16_t>(sample < 0 ? -sample : sample);
    if (level > peak) peak = level;
  }

  // A transmitter can consume pcm[0..frames) as little-endian PCM16 bytes.
  // Keep capture and network backpressure separate in a streaming application.
  if (millis() - lastReportAt >= 1000) {
    Serial.printf("Captured %lu frames/s, peak %u\n",
                  static_cast<unsigned long>(capturedFrames), peak);
    capturedFrames = 0;
    peak = 0;
    lastReportAt = millis();
  }
}
