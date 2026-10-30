#pragma once

#include "UnitDevlabCodec.h"
#include <DevLab_I2SCodecStream.h>

namespace unit_devlab {

// Captures signed 16-bit mono PCM from the ES8311 ADC. The codec's DOUT pin
// must be connected to the ESP32 I2S data input (pinout.data_in).
class ES8311Microphone {
 public:
  explicit ES8311Microphone(TwoWire &wire)
      : wire_(wire), board_(AudioDriverES8311, pins_), stream_(board_) {}

  ES8311Microphone(const ES8311Microphone &) = delete;
  ES8311Microphone &operator=(const ES8311Microphone &) = delete;

  // The same pinout may be started again after end(). gain is in [0, 1].
  bool begin(const ES8311Pinout &pinout, int sampleRate = 16000,
             float gain = 0.5f) {
    if (active_ || sampleRate <= 0 || gain < 0.0f || gain > 1.0f ||
        pinout.data_in < 0) {
      return false;
    }
    if (pinsReady_) {
      if (!samePinout(pinout_, pinout)) return false;
    } else {
      if (!configureES8311(pins_, pinout, wire_)) return false;
      pinout_ = pinout;
      pinsReady_ = true;
    }

    auto config = stream_.defaultConfig(RX_MODE);
    config.copyFrom(AudioInfo(sampleRate, 2, 16));
    config.input_device = ADC_INPUT_LINE1;
    config.output_device = DAC_OUTPUT_NONE;
    config.is_master = true;
    config.sd_active = false;
    if (!stream_.begin(config)) {
      stream_.end();
      return false;
    }
    static_cast<audio_tools::I2SDriver *>(stream_.driver())
        ->setWaitTimeReadMs(40);
    if (!stream_.setInputVolume(gain)) {
      stream_.end();
      return false;
    }
    active_ = true;
    sampleRate_ = sampleRate;
    return true;
  }

  void end() {
    if (!active_) return;
    stream_.end();
    active_ = false;
  }

  bool isActive() const { return active_; }
  int sampleRate() const { return sampleRate_; }

  bool setInputGain(float gain) {
    return active_ && gain >= 0.0f && gain <= 1.0f &&
           stream_.setInputVolume(gain);
  }

  // Returns up to 256 mono PCM16 samples per call.
  size_t readMono(int16_t *output, size_t maxFrames) {
    if (!active_ || output == nullptr || maxFrames == 0) return 0;
    const size_t requested = maxFrames < kBlockFrames ? maxFrames : kBlockFrames;
    const size_t bytes = stream_.readBytes(
        reinterpret_cast<uint8_t *>(interleaved_),
        requested * 2 * sizeof(int16_t));
    const size_t frames = bytes / (2 * sizeof(int16_t));
    if (frames == 0) return 0;

    uint32_t energy[2] = {0, 0};
    for (size_t i = 0; i < frames; ++i) {
      for (size_t channel = 0; channel < 2; ++channel) {
        const int32_t sample = interleaved_[2 * i + channel];
        energy[channel] += sample < 0 ? -sample : sample;
      }
    }
    const size_t channel = energy[1] > energy[0] ? 1 : 0;
    for (size_t i = 0; i < frames; ++i) {
      output[i] = interleaved_[2 * i + channel];
    }
    return frames;
  }

 private:
  static constexpr size_t kBlockFrames = 256;

  static bool samePinout(const ES8311Pinout &a, const ES8311Pinout &b) {
    return a.sda == b.sda && a.scl == b.scl && a.mclk == b.mclk &&
           a.bclk == b.bclk && a.ws == b.ws &&
           a.data_out == b.data_out && a.data_in == b.data_in &&
           a.i2c_address == b.i2c_address &&
           a.i2c_frequency == b.i2c_frequency &&
           a.i2c_active == b.i2c_active;
  }

  TwoWire &wire_;
  DriverPins pins_;
  AudioBoard board_;
  I2SCodecStream stream_;
  ES8311Pinout pinout_{};
  int16_t interleaved_[kBlockFrames * 2]{};
  int sampleRate_ = 0;
  bool pinsReady_ = false;
  bool active_ = false;
};

}  // namespace unit_devlab
