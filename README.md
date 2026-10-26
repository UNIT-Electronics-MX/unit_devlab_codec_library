# UNIT DevLab ES codec library

Arduino library for ES-series codecs in UNIT DevLab audio projects. The current implementation configures ES8311 pins and uses the ES8311 driver in the UNIT DevLab Audio Driver fork. Other ES codecs are not implemented yet. The MP3 examples also use the UNIT DevLab Audio Tools and LibHelix forks. This library does not copy the codec driver or decoder implementation.

## Required libraries

Install each fork from its repository in a separate folder inside your Arduino `libraries` directory:

| Arduino library | Repository |
|---|---|
| `DevLab_AudioDriver` | [unit_devlab_fork_arduino_audio_driver](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_driver) |
| `DevLab_AudioTools` | [unit_devlab_fork_arduino_audio_tools](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_tools) |
| `DevLab_LibHelix` | [unit_devlab_fork_arduino_libhelix](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_libhelix) |

The sine wave example needs Audio Driver and Audio Tools. The MP3 examples need all three forks. The OLED examples also need Adafruit GFX and Adafruit SSD1306. The encoder volume example needs Adafruit NeoPixel on ESP32-C6.

The unique `DevLab_*.h` headers select these libraries even when the original libraries are installed. The longer `unit_devlab_fork_*.h` headers remain available for existing sketches. Install the forks from GitHub until they are registered in Arduino Library Manager. Accordingly, `library.properties` does not declare automatic dependencies yet.

Use `<DevLab_AudioSourceSD.h>` for SD input, `<DevLab_AudioSource.h>` when extending the base source class, and `<DevLab_CodecMP3Helix.h>` for MP3 decoding. These headers keep Audio Tools' internal file layout out of sketches.

## Usage

```cpp
#include <DevLab_Codec_ES.h>
#include <DevLab_I2SCodecStream.h>

TwoWire codecWire(0);
DriverPins codecPins;
AudioBoard codecBoard(AudioDriverES8311, codecPins);

void setup() {
  const unit_devlab::ES8311Pinout pins{
      21, 22, 0, 4, 16, 15, -1, 0x18, 100000};
  if (!unit_devlab::configureES8311(codecPins, pins, codecWire)) {
    // Invalid pins or pins already registered.
    return;
  }
  // Initialize codecBoard through I2SCodecStream::begin(), as in the examples.
}
```

`configureES8311` registers the pins but does not initialize the codec. `I2SCodecStream::begin()` initializes I2S and `AudioBoard`; do not call `AudioBoard::begin()` before starting the stream. If I2C is already running and shared with an OLED or controls, set `i2c_active=false` in `ES8311Pinout` before registering the pins.

`DevLab_ES8311.h` remains available as a compatibility header for existing sketches.

## Examples

| Sketch | Purpose | Additional hardware |
|---|---|---|
| `es8311_sine_wave` | Generate a tone to check ES8311 and I2S on ESP32-WROOM or ESP32-C6 | None |
| `player_mp3_volume` | Basic MP3 player with volume control | microSD |
| `player_mp3_uart` | Control playback over the serial port | microSD |
| `player_mp3_oled` | MP3 player with display | microSD, OLED, optional encoder |
| `player_mp3_oled_encoder_volume` | Display and encoder volume control | microSD, OLED, UE0172 encoder |
| `player_mp3_oled_slider` | Display and slider on a second I2C bus | microSD, OLED, encoder, slider |

The sine wave sketch selects these pins at compile time:

| Board | I2C SDA | I2C SCL | I2S MCLK | I2S BCLK | I2S WS | ES8311 DIN |
|---|---:|---:|---:|---:|---:|---:|
| ESP32-WROOM | 21 | 22 | 0 | 4 | 16 | 15 |
| ESP32-C6 | 22 | 23 | 21 | 3 | 15 | 20 |

Each sketch uses the pin mapping of its DualMCU prototype. Pin assignments may differ between ESP32 and ESP32-C6 variants. Check your board wiring before uploading. All six sketches compiled with ESP32 Arduino core 3.3.1. The encoder volume sketch also compiled for ESP32-C6, and the sine wave sketch compiled for ESP32-S3. Audio playback, shared buses, and controls still need hardware testing.

From the directory containing the four repositories:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --library ./unit_devlab_codec_library \
  --library ./unit_devlab_fork_arduino_audio_driver \
  --library ./unit_devlab_fork_arduino_audio_tools \
  --library ./unit_devlab_fork_arduino_libhelix \
  ./unit_devlab_codec_library/examples/player_mp3_uart
```

## Credits and license

ES codec library and example integration: Cesar Bautista / UNIT Electronics. Adrián Rabadán contributed to the initial DualMCU version. Audio Driver, Audio Tools, and the LibHelix adaptation are projects by Phil Schatzmann. Helix includes work by RealNetworks and other contributors. Each fork retains its own license. This repository is distributed under GPL-3.0; see `LICENSE`.

The initial revision of this repository contained a copy of `DevLab_BMI323`. That revision remains in Git history. The `main` branch now contains the ES codec library, currently supporting ES8311.
