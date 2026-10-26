#include "UnitDevlabCodec.h"

namespace unit_devlab {

bool configureES8311(audio_driver::DriverPins& pins,
                     const ES8311Pinout& pinout, TwoWire& wire) {
  if (pinout.sda < 0 || pinout.scl < 0 || pinout.bclk < 0 ||
      pinout.ws < 0 || pinout.data_out < 0 ||
      pinout.sda == pinout.scl || pinout.bclk == pinout.ws ||
      pinout.bclk == pinout.data_out || pinout.ws == pinout.data_out ||
      pinout.i2c_address < 1 || pinout.i2c_address > 0x7f ||
      pinout.i2c_frequency == 0) {
    return false;
  }

  const bool i2c_ok = pins.addI2C(audio_driver::PinFunction::CODEC,
                                   pinout.scl, pinout.sda,
                                   pinout.i2c_address,
                                   pinout.i2c_frequency, wire,
                                   pinout.i2c_active);
  const bool i2s_ok = pins.addI2S(audio_driver::PinFunction::CODEC,
                                   pinout.mclk, pinout.bclk, pinout.ws,
                                   pinout.data_out, pinout.data_in);
  return i2c_ok && i2s_ok;
}

}  // namespace unit_devlab
