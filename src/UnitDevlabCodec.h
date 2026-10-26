#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <unit_devlab_fork_audio_driver.h>

namespace unit_devlab {

// Pin order matches the DualMCU sketches: I2C, then I2S, then bus settings.
struct ES8311Pinout {
  int sda;
  int scl;
  int mclk;
  int bclk;
  int ws;
  int data_out;
  int data_in;
  int i2c_address = 0x18;
  uint32_t i2c_frequency = 100000;
  bool i2c_active = true;
};

// Register the codec pins before calling AudioBoard::begin() or
// I2SCodecStream::begin(). The latter initializes the board as well.
bool configureES8311(audio_driver::DriverPins& pins,
                     const ES8311Pinout& pinout, TwoWire& wire);

}  // namespace unit_devlab
