// Send ES8311 microphone audio as framed PCM16 over Serial.
// Receive it with extras/serial_pcm_to_wav.py; do not use Serial Monitor.
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
constexpr int kSerialBaud = 921600;
constexpr size_t kFramesPerPacket = 256;
constexpr size_t kPcmBytesPerPacket = kFramesPerPacket * sizeof(int16_t);

TwoWire codecWire(0);
unit_devlab::ES8311Microphone microphone(codecWire);
int16_t pcm[kFramesPerPacket];
size_t bufferedFrames = 0;
uint32_t packetSequence = 0;

class SilentOutput : public Print {
 public:
  size_t write(uint8_t) override { return 1; }
};
SilentOutput silentOutput;

void put16(uint8_t *dest, uint16_t value) {
  dest[0] = static_cast<uint8_t>(value);
  dest[1] = static_cast<uint8_t>(value >> 8);
}

void put32(uint8_t *dest, uint32_t value) {
  for (int i = 0; i < 4; ++i) dest[i] = static_cast<uint8_t>(value >> (8 * i));
}

uint16_t crc16(const uint8_t *data, size_t length) {
  uint16_t crc = 0xffff;
  for (size_t i = 0; i < length; ++i) {
    crc ^= static_cast<uint16_t>(data[i]) << 8;
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 0x8000) ? static_cast<uint16_t>((crc << 1) ^ 0x1021)
                           : static_cast<uint16_t>(crc << 1);
    }
  }
  return crc;
}

bool writeAll(const uint8_t *data, size_t length) {
  size_t sent = 0;
  uint32_t lastProgressAt = millis();
  while (sent < length) {
    const size_t count = Serial.write(data + sent, length - sent);
    if (count > 0) {
      sent += count;
      lastProgressAt = millis();
    } else if (millis() - lastProgressAt > 100) {
      return false;
    } else {
      delay(1);
    }
  }
  return true;
}

void sendPacket() {
  // 12-byte header: "DLM1", sequence, frame count, CRC16-CCITT of PCM payload.
  uint8_t header[12] = {'D', 'L', 'M', '1'};
  put32(header + 4, packetSequence++);
  put16(header + 8, kFramesPerPacket);
  const auto *payload = reinterpret_cast<const uint8_t *>(pcm);
  put16(header + 10, crc16(payload, kPcmBytesPerPacket));
  if (writeAll(header, sizeof(header))) {
    writeAll(payload, kPcmBytesPerPacket);
  }
}

void setup() {
  Serial.begin(kSerialBaud);
  // Keep diagnostics out of the binary audio stream.
  AudioLogger::instance().begin(silentOutput, AudioLogger::Error);
  AudioDriverLogger.begin(silentOutput, AudioDriverLogLevel::Error);
  const unit_devlab::ES8311Pinout pins{
      kSda, kScl, kMclk, kBclk, kWs, kCodecDin, kCodecDout, 0x18, 100000};
  if (!microphone.begin(pins, kSampleRate, kCodecInputGain)) {
    while (true) delay(1000);
  }
}

void loop() {
  const size_t frames = microphone.readMono(pcm + bufferedFrames,
                                            kFramesPerPacket - bufferedFrames);
  bufferedFrames += frames;
  if (bufferedFrames == kFramesPerPacket) {
    sendPacket();
    bufferedFrames = 0;
  }
}
