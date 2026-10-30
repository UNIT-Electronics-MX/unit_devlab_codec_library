# UNIT DevLab ES codec library

Arduino library for ES-series codecs in UNIT DevLab audio projects. The current implementation configures ES8311 pins and uses the ES8311 driver in the UNIT DevLab Audio Driver fork. Other ES codecs are not implemented yet. The MP3 examples also use the UNIT DevLab Audio Tools and LibHelix forks. This library does not copy the codec driver or decoder implementation.

## Required libraries

Install these repositories in separate folders inside your Arduino `libraries` directory:

| Arduino library | Repository |
|---|---|
| `DevLab_AudioDriver` | [unit_devlab_fork_arduino_audio_driver](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_driver) |
| `DevLab_AudioTools` | [unit_devlab_fork_arduino_audio_tools](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_tools) |
| `DevLab_LibHelix` | [unit_devlab_fork_arduino_libhelix](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_libhelix) |
| `DevLab_BL` (Pomodoro eyes example) | [unit_devlab_bl_library](https://github.com/UNIT-Electronics-MX/unit_devlab_bl_library) |

The sine wave and microphone examples need Audio Driver and Audio Tools. The MP3 examples need all three forks. The OLED examples also need Adafruit GFX and Adafruit SSD1306. The Pomodoro example also needs `DevLab_BL` for its eyes display, without enabling Bluetooth. Both encoder examples need Adafruit NeoPixel on ESP32-C6.

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

### Microphone capture

Include `<DevLab_ES8311Microphone.h>` to capture audio from the ES8311 ADC. Connect the codec's digital DOUT signal to the ESP32 I2S data input. In the supplied ESP32-WROOM and ESP32-C6 wiring, this signal reaches GPIO5. GPIO5 is an I2S data input here, not an analog microphone input.

`ES8311Microphone::begin(pinout, sampleRate, gain)` initializes the codec for RX and sets the input gain from 0.0 to 1.0. `readMono()` returns up to 256 signed 16-bit mono samples per call at the configured sample rate. It selects the more active I2S channel for each block because the mono ADC may appear in either slot. The serial example sends `frames * sizeof(int16_t)` bytes from the returned buffer as little-endian PCM16. Call `end()` when capture stops. The same pinout can be restarted with `begin()`.

The `es8311_mic_capture` example checks the ADC by printing the frame rate and peak sample level. It uses 16 kHz and leaves the PCM buffer available for serial transmission.

To save actual microphone audio on a computer, upload `es8311_mic_serial_pcm`. This sketch sends 16 kHz mono PCM16 in 256-sample packets at 921600 baud. Each packet has a 12-byte header with the `DLM1` marker, sequence number, frame count, and CRC16-CCITT. Diagnostic text is excluded from the binary stream. Close Arduino Serial Monitor before opening the port with the receiver:

```sh
python3 -m pip install pyserial
python3 extras/serial_pcm_to_wav.py /dev/ttyACM0 microphone.wav --seconds 10
```

Run the command from the codec library directory. Use `/dev/ttyUSB0` instead if that is your board's port. The receiver writes a playable WAV file, reports invalid or missing packets and the input peak/RMS before digital gain, and inserts silence for short packet gaps. The frame-rate and peak lines from `es8311_mic_capture` contain no PCM samples and cannot reconstruct earlier audio.

The microphone examples now set the ES8311 input gain to `1.0f` for this wiring. Record once without `--gain` after uploading the updated sketch. If the WAV is still quiet, try `--gain 2` or `--gain 4` when running the receiver. Digital gain multiplies the PCM samples and clips values outside the 16-bit range. Reduce `kCodecInputGain` in the sketch if loud sounds clip.

### Browser listening through a computer

The computer can receive the same framed PCM over USB Serial and serve live audio to a browser. Keep `es8311_mic_serial_pcm` on the ESP32; no new firmware is needed for this bridge. Close Arduino Serial Monitor and the WAV receiver before starting the bridge:

```sh
python3 extras/serial_pcm_web.py /dev/ttyACM0
```

Run this command from the codec library directory, then open `http://127.0.0.1:8000/` on the computer and press **Start listening**. The page shows received, missing, and invalid packet counts and has a playback gain slider. For a phone on the same local Wi-Fi network, start the server with `--host 0.0.0.0`, find the computer's address with `hostname -I`, and open `http://<computer-LAN-IP>:8000/` on the phone. That option makes the live microphone stream reachable on the local network without authentication. The browser page and its raw PCM endpoint run on the computer; the USB connection carries audio from the ESP32. The bridge drops old queued audio if a browser becomes slow so that playback stays close to live input.

## Examples

| Sketch | Purpose | Additional hardware |
|---|---|---|
| `es8311_sine_wave` | Generate a tone to check ES8311 and I2S on ESP32-WROOM or ESP32-C6 | None |
| `es8311_mic_capture` | Capture 16 kHz mono PCM and report microphone levels on ESP32-WROOM or ESP32-C6 | ES8311 microphone input and DOUT wired to GPIO5 |
| `es8311_mic_serial_pcm` | Send framed PCM16 over Serial for WAV recording on a computer | ES8311 microphone input and DOUT wired to GPIO5 |
| `player_mp3_volume` | Basic MP3 player with volume control | microSD |
| `player_mp3_uart` | Control playback over the serial port | microSD |
| `player_mp3_oled` | MP3 player with display | microSD, OLED, optional encoder |
| `player_mp3_oled_encoder_volume` | Display and encoder volume control | microSD, OLED, UE0172 encoder |
| `player_mp3_oled_encoder_pomodoro` | MP3 player with a Pomodoro timer and Kubi eyes | microSD, OLED, UE0172 encoder |
| `player_mp3_oled_slider` | Display and slider on a second I2C bus | microSD, OLED, encoder, slider |

The sine wave sketch selects these pins at compile time:

| Board | I2C SDA | I2C SCL | I2S MCLK | I2S BCLK | I2S WS | ES8311 DIN | ES8311 DOUT for microphone |
|---|---:|---:|---:|---:|---:|---:|---:|
| ESP32-WROOM | 21 | 22 | 0 | 4 | 16 | 15 | 5 |
| ESP32-C6 | 22 | 23 | 21 | 3 | 15 | 20 | 5 |

Each sketch uses the pin mapping of its DualMCU prototype. Pin assignments may differ between ESP32 and ESP32-C6 variants. Check your board wiring before uploading. Both microphone examples compile for ESP32-WROOM and ESP32-C6 with ESP32 Arduino core 3.3.1; serial PCM recording to WAV was verified on ESP32-C6. The original seven sketches compiled with the same core. Both encoder variants also compiled for ESP32-C6, and the sine wave sketch compiled for ESP32-S3. Audio playback, shared buses, and controls still need hardware testing.

### Pomodoro controls

`player_mp3_oled_encoder_pomodoro` keeps MP3 playback active while its timer runs. Hold the encoder button for 800 ms to cycle through the player, Pomodoro timer, and eyes screens. On the timer and eyes screens, click to start or pause and double-click to reset the current phase. Turning the encoder while a focus phase is stopped sets its length from 1 to 60 minutes; turning it while running or during a break adjusts music volume.

The default cycle is 25 minutes of focus, 5 minutes of short break, and a 15-minute long break after every four completed focus phases. Phases advance automatically. The eyes screen uses `DevLabEyes` from `DevLab_BL` with `setEyesOnly(true)`: happy eyes during focus, a wink while focus is paused, and sleepy eyes during breaks. Only eyes appear; no mouth or sleep symbols are drawn. The eyes change when the timer state changes and remain static between changes to avoid frequent full-screen I2C transfers during audio playback. Transitions appear on the OLED and Serial; this version does not play a separate chime or start BLE. It needs a microSD card with an MP3 file, just like the encoder volume example, and uses that example's ESP32-WROOM and ESP32-C6 pin mappings.

From the directory containing the codec library and the three audio forks:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --library ./unit_devlab_codec_library \
  --library ./unit_devlab_fork_arduino_audio_driver \
  --library ./unit_devlab_fork_arduino_audio_tools \
  --library ./unit_devlab_fork_arduino_libhelix \
  ./unit_devlab_codec_library/examples/player_mp3_uart
```

To compile the Pomodoro eyes example, also add `--library ./unit_devlab_bl_library` and use `./unit_devlab_codec_library/examples/player_mp3_oled_encoder_pomodoro` as the sketch path.

## Credits and license

ES codec library and example integration: Cesar Bautista / UNIT Electronics. Adrián Rabadán contributed to the initial DualMCU version. Audio Driver, Audio Tools, and the LibHelix adaptation are projects by Phil Schatzmann. Helix includes work by RealNetworks and other contributors. Each fork retains its own license. This repository is distributed under GPL-3.0; see `LICENSE`.

The initial revision of this repository contained a copy of `DevLab_BMI323`. That revision remains in Git history. The `main` branch now contains the ES codec library, currently supporting ES8311.
