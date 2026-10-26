// Reproductor MP3 por microSD con ES8311 y OLED SSD1306 128x64.
// OLED y codec comparten I2C: SDA=21, SCL=22.
// Encoder UE0172 (0x31) y slider WS12xx (0x24): SDA=25, SCL=26.
// La interfaz corre en el nucleo 0; el audio conserva el loop del nucleo 1.
#include <stdlib.h>
#include <atomic>
#include <vector>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <UnitDevlabCodecPlayback.h>

#define SDAPIN       21
#define SCLPIN       22
#define I2CSPEED     400000
#define ES8311_ADDR  0x18
#define MCLKPIN       0
#define BCLKPIN       4
#define WSPIN        16
#define DOPIN        15  // ESP32 -> DSDIN del ES8311
#define DIPIN        -1

// 0: encoder junto al ES8311 y OLED en 21/22.
// 1: encoder en Wire1 junto al slider (SDA=25, SCL=26).
#ifndef ENCODER_OWN_I2C
#define ENCODER_OWN_I2C 1
#endif
#define ENCODER_SDAPIN 25
#define ENCODER_SCLPIN 26

const int sdChipSelect = SS;
const char *startFilePath = "/";
const char *extension = ".mp3";
constexpr uint8_t kEncoderAddress = 0x31;
constexpr uint8_t kEncoderIdCommand = 0x00;
constexpr uint8_t kEncoderPositionCommand = 0x80;
constexpr uint8_t kEncoderButtonCommand = 0x81;
constexpr unsigned long kEncoderPollMs = 20;
constexpr unsigned long kEncoderRetryMs = 2000;
constexpr unsigned long kDoubleClickMs = 350;
constexpr uint16_t kI2CTimeoutMs = 5;
constexpr uint8_t kSliderAddress = 0x24;
constexpr uint8_t kSliderIdCommand = 0x00;
constexpr uint8_t kSliderAdcCommand = 0x60;
constexpr uint8_t kSliderPixelCommand = 0xB0;

constexpr uint8_t kSliderClearCommand = 0xB2;
constexpr uint8_t kSliderBrightnessCommand = 0xB5;
constexpr uint8_t kSliderPixelCount = 16;
constexpr uint8_t kSliderBrightness = 96;
constexpr unsigned long kSliderReadWaitMs = 30;
constexpr unsigned long kSliderSampleMs = 100;
constexpr unsigned long kSliderRetryMs = 2000;
constexpr unsigned long kVolumeDisplayMs = 2500;
constexpr unsigned long kDiscoFrameMs = 85;

// AudioSourceSD recorre toda la microSD cada vez que se elige una pista.
// Esta fuente la indexa una sola vez y guarda las rutas en RAM.
class AudioSourceSDList : public AudioSource {
 public:
  AudioSourceSDList(const char *startPath, const char *ext)
      : startPath(startPath), extension(ext) {}

#if __has_include("AudioTools/Disk/AudioSource.h")
  bool begin() override {
#else
  void begin() override {
#endif
    if (paths.empty()) scan(startPath);
    position = 0;
#if __has_include("AudioTools/Disk/AudioSource.h")
    return true;
#endif
  }

  Stream *nextStream(int offset) override { return selectStream(position + offset); }

  Stream *selectStream(int index) override {
    if (index < 0 || index >= (int)paths.size()) return nullptr;
    position = index;
    file.close();
    file = SD.open(paths[index]);
    return file ? &file : nullptr;
  }

  Stream *selectStream(const char *path) override {
    file.close();
    file = SD.open(path);
    return file ? &file : nullptr;
  }

  int index() const { return position; }
  const char *toStr() const { return paths.empty() ? nullptr : paths[position].c_str(); }
  long size() const { return paths.size(); }
  const String &path(int index) const { return paths[index]; }

 private:
  const char *startPath;
  const char *extension;
  std::vector<String> paths;
  int position = 0;
  File file;

  bool hasExtension(const char *name) const {
    const size_t nameLength = strlen(name);
    const size_t extLength = strlen(extension);
    return nameLength >= extLength &&
           strcasecmp(name + nameLength - extLength, extension) == 0;
  }

  void scan(const char *dirname, int depth = 0) {
    File directory = SD.open(dirname);
    if (!directory || !directory.isDirectory()) return;
    File entry;
    while ((entry = directory.openNextFile())) {
      if (entry.name()[0] != '.') {
        if (entry.isDirectory()) {
          const String subdirectory = entry.path();
          entry.close();
          if (depth < 8) scan(subdirectory.c_str(), depth + 1);
          continue;
        }
        if (hasExtension(entry.name())) paths.push_back(entry.path());
      }
      entry.close();
    }
  }
};

TwoWire myWire(0);
TwoWire controlWire(1);
// Adafruit restaura el reloj al valor clkAfter tras cada refresco.
Adafruit_SSD1306 oled(128, 64, &myWire, -1, I2CSPEED, I2CSPEED);
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);
AudioSourceSDList source(startFilePath, extension);
MP3DecoderHelix decoder;
std::atomic<uint8_t> musicPeakLevel{0};

// Volumen pedido (0.0-1.0); MusicLevelMeter se acerca a el muestra a muestra.
std::atomic<float> targetGain{0.8f};

// Aplica el volumen y mide el PCM que realmente sale al codec.
class MusicLevelMeter : public VolumeMeter {
 public:
  using VolumeMeter::write;
  explicit MusicLevelMeter(AudioStream &output) : VolumeMeter(output) {}

  size_t write(const uint8_t *data, size_t len) override {
    // Un salto de ganancia en cada lectura del slider se oye como zumbido;
    // la rampa recorre todo el rango en ~50 ms a 44.1 kHz.
    static constexpr float kGainStep = 1.0f / 2205.0f;
    const int16_t *input = reinterpret_cast<const int16_t *>(data);
    const size_t frames = len / 4;
    int16_t buffer[256];
    size_t done = 0;
    while (done < frames) {
      const size_t count = min(frames - done, sizeof(buffer) / 4);
      const float target = targetGain.load(std::memory_order_relaxed);
      for (size_t i = 0; i < count; ++i) {
        if (gain < target) gain = min(gain + kGainStep, target);
        else if (gain > target) gain = max(gain - kGainStep, target);
        buffer[2 * i] = (int16_t)(input[2 * (done + i)] * gain);
        buffer[2 * i + 1] = (int16_t)(input[2 * (done + i) + 1] * gain);
      }
      writeAll(reinterpret_cast<const uint8_t *>(buffer), count * 4);
      updatePeak();
      done += count;
    }
    if (len > frames * 4) writeAll(data + frames * 4, len - frames * 4);
    return len;
  }

 private:
  float gain = 0.0f;

  void writeAll(const uint8_t *data, size_t len) {
    while (len > 0) {
      const size_t written = VolumeMeter::write(data, len);
      if (written == 0) return;
      data += written;
      len -= written;
    }
  }

  void updatePeak() {
    // Umbrales logaritmicos: los pasajes suaves tambien mueven la barra.
    static constexpr float kPeaks[16] = {
        0.004f, 0.006f, 0.009f, 0.013f,
        0.019f, 0.027f, 0.038f, 0.054f,
        0.076f, 0.108f, 0.153f, 0.216f,
        0.306f, 0.433f, 0.612f, 0.866f};
    const float amplitude = volumeRatio();
    uint8_t level = 0;
    while (level < kSliderPixelCount && amplitude >= kPeaks[level]) ++level;
    uint8_t previous = musicPeakLevel.load(std::memory_order_relaxed);
    while (level > previous &&
           !musicPeakLevel.compare_exchange_weak(previous, level,
                                                  std::memory_order_relaxed)) {}
  }
};

MusicLevelMeter musicMeter(i2s);
AudioPlayer player(source, musicMeter, decoder);

enum class Screen { Player, Songs };
Screen screen = Screen::Player;

bool oledReady = false;
bool playerReady = false;
bool isPaused = false;
bool isMuted = false;
std::atomic<bool> displayDirty{true};
// Interruptores de diagnostico: se cambian desde Serial en el nucleo de audio
// y los consulta la tarea de interfaz antes de cada acceso I2C.
std::atomic<bool> encoderPollingEnabled{true};
std::atomic<bool> sliderPollingEnabled{true};
std::atomic<bool> sliderLedsEnabled{true};
std::atomic<bool> oledRefreshEnabled{true};
float currentVolume = 0.8f;
bool encoderAvailable = false;
bool encoderSynced = false;
bool lastEncoderButton = false;
bool buttonTurned = false;
bool clickPending = false;
int32_t lastEncoderPosition = 0;
int32_t menuSelection = 0;
uint8_t encoderFailures = 0;
unsigned long lastEncoderPoll = 0;
unsigned long lastEncoderProbe = 0;
unsigned long firstClickAt = 0;
bool sliderAvailable = false;
bool sliderReadPending = false;
uint8_t sliderFailures = 0;
uint8_t sliderDisplayedLevel = 0;
uint8_t sliderTargetLevel = 0;
uint8_t sliderVolumeLevel = 0;
bool sliderLedsConfigured = false;
bool sliderDiscoMode = false;
int sliderLastVolumePercent = -1;
unsigned long sliderAdcSentAt = 0;
unsigned long sliderNextSampleAt = 0;
unsigned long sliderNextProbeAt = 0;
unsigned long sliderNextVisualAt = 0;
std::atomic<uint32_t> lastVolumeActivityMs{0};
uint32_t lastAudioCopyEndUs = 0;
uint32_t maxAudioLoopGapUs = 0;
uint32_t maxAudioCopyTimeUs = 0;
uint32_t audioCopyCalls = 0;
uint32_t audioEmptyCopies = 0;
long mp3Count = 0;
String currentFile;
String songTitle;
String songArtist;
unsigned int titlePage = 0;
unsigned long lastPageChange = 0;
unsigned long lastDraw = 0;
String lastUiTitle;

struct UiSnapshot {
  char title[160];
  char artist[64];
  int32_t trackIndex;
  int volumePercent;
  bool paused;
  bool muted;
};

enum class ControlType : uint8_t { PlayIndex, TogglePause, SetVolume };
struct ControlCommand {
  ControlType type;
  int32_t value;
};

UiSnapshot uiSnapshot{};
portMUX_TYPE uiStateMux = portMUX_INITIALIZER_UNLOCKED;
QueueHandle_t controlQueue = nullptr;
TaskHandle_t uiTaskHandle = nullptr;

void publishUiState() {
  portENTER_CRITICAL(&uiStateMux);
  songTitle.toCharArray(uiSnapshot.title, sizeof(uiSnapshot.title));
  songArtist.toCharArray(uiSnapshot.artist, sizeof(uiSnapshot.artist));
  uiSnapshot.trackIndex = source.index();
  uiSnapshot.volumePercent = (int)(currentVolume * 100.0f + 0.5f);
  uiSnapshot.paused = isPaused;
  uiSnapshot.muted = isMuted;
  portEXIT_CRITICAL(&uiStateMux);
  displayDirty = true;
}

UiSnapshot getUiState() {
  UiSnapshot state;
  portENTER_CRITICAL(&uiStateMux);
  state = uiSnapshot;
  portEXIT_CRITICAL(&uiStateMux);
  return state;
}

bool sendControl(ControlType type, int32_t value = 0) {
  if (controlQueue == nullptr) return false;
  const ControlCommand command{type, value};
  return xQueueSend(controlQueue, &command, 0) == pdTRUE;
}

uint8_t findOledAddress() {
  const uint8_t candidates[] = {0x3C, 0x3D};
  for (uint8_t address : candidates) {
    myWire.beginTransmission(address);
    if (myWire.endTransmission() == 0) return address;
  }
  return 0;
}

TwoWire &encoderBus() {
#if ENCODER_OWN_I2C
  return controlWire;
#else
  return myWire;
#endif
}

bool readEncoderCommand(uint8_t command, uint8_t *reply, uint8_t length) {
  TwoWire &bus = encoderBus();
  bus.beginTransmission(kEncoderAddress);
  bus.write(command);
  if (bus.endTransmission() != 0) return false;
  if (bus.requestFrom(kEncoderAddress, length) != length) return false;
  for (uint8_t i = 0; i < length; ++i) reply[i] = bus.read();
  return true;
}

bool detectEncoder() {
  uint8_t id[2];
  encoderAvailable = readEncoderCommand(kEncoderIdCommand, id, sizeof(id)) &&
                     id[0] == 0x11 && id[1] == 0x01;
  if (encoderAvailable) {
    encoderSynced = false;
    encoderFailures = 0;
    Serial.println("Encoder UE0172 detectado en 0x31");
  }
  return encoderAvailable;
}

bool writeSliderByte(uint8_t value) {
  controlWire.beginTransmission(kSliderAddress);
  controlWire.write(value);
  if (controlWire.endTransmission() != 0) return false;
  delay(3);  // El PY32 recibe cada byte DDP en una escritura independiente.
  return true;
}

bool setSliderPixel(uint8_t position, bool on, bool disco) {
  const uint8_t index = kSliderPixelCount - 1U - position;
  static const uint8_t kDiscoColors[6][3] = {
      {255, 0, 80}, {160, 0, 255}, {0, 80, 255},
      {0, 255, 120}, {255, 180, 0}, {255, 30, 0}};
  const uint8_t color = (position * 6U) / kSliderPixelCount;
  const uint8_t red = !on ? 0 : (disco ? kDiscoColors[color][0] :
                                           (position >= 10U ? 255U : 0U));
  const uint8_t green = !on ? 0 : (disco ? kDiscoColors[color][1] :
                                             (position < 10U ? 255U :
                                              (position < 14U ? 160U : 0U)));
  const uint8_t blue = on && disco ? kDiscoColors[color][2] : 0;
  return writeSliderByte(kSliderPixelCommand) && writeSliderByte(index) &&
         writeSliderByte(red) && writeSliderByte(green) && writeSliderByte(blue);
}

bool detectSlider() {
  uint8_t id[2];
  controlWire.beginTransmission(kSliderAddress);
  controlWire.write(kSliderIdCommand);
  if (controlWire.endTransmission() != 0) return false;
  delay(5);  // Tiempo de preparacion indicado por el ejemplo del WS12xx.
  if (controlWire.requestFrom(kSliderAddress, (uint8_t)2) != 2) return false;
  id[0] = controlWire.read();
  id[1] = controlWire.read();
  if (id[0] != 0x00 || id[1] != 0x04) return false;
  sliderLedsConfigured = false;
  if (sliderLedsEnabled.load()) {
    if (!writeSliderByte(kSliderBrightnessCommand) ||
        !writeSliderByte(kSliderBrightness) ||
        !writeSliderByte(kSliderClearCommand)) return false;
    sliderLedsConfigured = true;
  }
  sliderAvailable = true;
  sliderReadPending = false;
  sliderFailures = 0;
  sliderDisplayedLevel = 0;
  sliderTargetLevel = 0;
  sliderVolumeLevel = 0;
  sliderDiscoMode = false;
  sliderLastVolumePercent = -1;
  sliderNextSampleAt = millis();
  sliderNextVisualAt = millis();
  lastVolumeActivityMs.store(millis(), std::memory_order_relaxed);
  Serial.println("Slider WS12xx detectado en 0x24");
  return true;
}

void disconnectSlider() {
  sliderAvailable = false;
  sliderReadPending = false;
  sliderNextProbeAt = millis() + kSliderRetryMs;
  Serial.println("Slider desconectado; el reproductor continua");
}

void sliderReadFailed() {
  sliderReadPending = false;
  sliderNextSampleAt = millis() + kSliderSampleMs;
  if (++sliderFailures >= 3) disconnectSlider();
}

void pollSlider() {
  const unsigned long now = millis();
  if (!sliderAvailable) {
    if ((int32_t)(now - sliderNextProbeAt) >= 0) {
      if (!detectSlider()) sliderNextProbeAt = millis() + kSliderRetryMs;
    }
    return;
  }

  if (sliderReadPending) {
    if (now - sliderAdcSentAt < kSliderReadWaitMs) return;
    sliderReadPending = false;
    if (controlWire.requestFrom(kSliderAddress, (uint8_t)2) != 2) {
      sliderReadFailed();
      return;
    }
    const uint16_t raw = (uint16_t)controlWire.read() |
                         ((uint16_t)controlWire.read() << 8);
    if (raw > 4095U) {
      sliderReadFailed();
      return;
    }
    sliderFailures = 0;
    sliderNextSampleAt = millis() + kSliderSampleMs;
    sliderVolumeLevel = raw == 0 ? 0 :
        ((uint32_t)raw * kSliderPixelCount + 4094U) / 4095U;
    const int percent = ((uint32_t)raw * 100U + 2047U) / 4095U;
    if (sliderLastVolumePercent < 0 ||
        abs(percent - sliderLastVolumePercent) >= 2 ||
        (percent == 0 && sliderLastVolumePercent != 0) ||
        (percent == 100 && sliderLastVolumePercent != 100)) {
      lastVolumeActivityMs.store(millis(), std::memory_order_relaxed);
      if (sendControl(ControlType::SetVolume, percent)) {
        sliderLastVolumePercent = percent;
      }
    }
    return;
  }

  if ((int32_t)(now - sliderNextSampleAt) >= 0) {
    controlWire.beginTransmission(kSliderAddress);
    controlWire.write(kSliderAdcCommand);
    if (controlWire.endTransmission() != 0) {
      sliderReadFailed();
    } else {
      sliderReadPending = true;
      sliderAdcSentAt = millis();
    }
    return;
  }

  // Actualiza como maximo un LED por vuelta de tarea para atender el encoder.
  if (!sliderLedsEnabled.load()) return;
  if (!sliderLedsConfigured) {
    if (!writeSliderByte(kSliderBrightnessCommand) ||
        !writeSliderByte(kSliderBrightness) ||
        !writeSliderByte(kSliderClearCommand)) {
      disconnectSlider();
      return;
    }
    sliderLedsConfigured = true;
    sliderDisplayedLevel = 0;
  }
  const bool disco = now - lastVolumeActivityMs.load(std::memory_order_relaxed) >=
                     kVolumeDisplayMs;
  if (disco != sliderDiscoMode) {
    if (!writeSliderByte(kSliderClearCommand)) {
      disconnectSlider();
      return;
    }
    sliderDisplayedLevel = 0;
    sliderDiscoMode = disco;
    sliderNextVisualAt = now;
  }
  if ((int32_t)(now - sliderNextVisualAt) >= 0) {
    const uint8_t peak = musicPeakLevel.exchange(0, std::memory_order_relaxed);
    if (disco) sliderTargetLevel = peak;
    sliderNextVisualAt = now + kDiscoFrameMs;
  }
  if (!disco) sliderTargetLevel = sliderVolumeLevel;
  if (sliderDisplayedLevel < sliderTargetLevel) {
    if (setSliderPixel(sliderDisplayedLevel, true, disco)) ++sliderDisplayedLevel;
    else disconnectSlider();
  } else if (sliderDisplayedLevel > sliderTargetLevel) {
    if (setSliderPixel(sliderDisplayedLevel - 1U, false, disco)) --sliderDisplayedLevel;
    else disconnectSlider();
  }
}

void showMessage(const char *message) {
  Serial.println(message);
  if (!oledReady) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.println("MP3 + ES8311");
  oled.setCursor(0, 20);
  oled.println(message);
  oled.display();
}

void listMp3Files() {
  for (long i = 0; i < mp3Count; ++i) {
    Serial.printf("%3ld %s\n", i + 1, source.path(i).c_str());
  }
}

String fileTitle(const String &path) {
  int slash = path.lastIndexOf('/');
  String result = path.substring(slash + 1);
  if (result.length() >= 4) {
    String suffix = result.substring(result.length() - 4);
    suffix.toLowerCase();
    if (suffix == ".mp3") result.remove(result.length() - 4);
  }
  result.replace('_', ' ');
  return result.length() ? result : "Sin titulo";
}

void syncTrack() {
  const char *path = source.toStr();
  if (path == nullptr) return;
  if (strcmp(path, currentFile.c_str()) == 0) return;
  currentFile = path;
  songTitle = fileTitle(currentFile);
  songArtist = "";
  publishUiState();
  Serial.print("Reproduciendo: ");
  Serial.println(currentFile);
}

void onMetadata(MetaDataType type, const char *value, int length) {
  if ((type != Title && type != Artist) || value == nullptr || length <= 0) return;
  String text;
  text.reserve(length + 1);
  for (int i = 0; i < length && value[i] != '\0'; ++i) text += value[i];
  text.trim();
  if (text.length() == 0) return;
  if (type == Title) {
    songTitle = text;
  } else {
    songArtist = text;
  }
  publishUiState();
}

void drawPlayer() {
  if (!oledReady) return;
  const UiSnapshot state = getUiState();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print("MP3  ");
  oled.print(state.trackIndex + 1);
  oled.print("/");
  oled.print(mp3Count);
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  const String title(state.title);
  const unsigned int offset = titlePage * 42;
  oled.setCursor(0, 15);
  oled.print(title.substring(offset, offset + 21));
  oled.setCursor(0, 26);
  oled.print(title.substring(offset + 21, offset + 42));
  oled.setCursor(0, 39);
  if (state.artist[0] != '\0') {
    oled.print(String(state.artist).substring(0, 21));
  } else {
    oled.print("microSD");
  }
  oled.setCursor(0, 53);
  oled.print(state.muted ? "MUTE" : (state.paused ? "PAUSA" : "PLAY"));
  oled.print("  Vol:");
  oled.print(state.volumePercent);
  oled.print("%");
  oled.display();
  lastDraw = millis();
}

void drawSongMenu() {
  if (!oledReady) return;
  const UiSnapshot state = getUiState();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print("CANCIONES ");
  oled.print(menuSelection + 1);
  oled.print("/");
  oled.print(mp3Count);
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  const int32_t first = (menuSelection / 4) * 4;
  for (int row = 0; row < 4 && first + row < mp3Count; ++row) {
    const int32_t index = first + row;
    oled.setCursor(0, 14 + row * 12);
    oled.print(index == menuSelection ? '>' : ' ');
    oled.print(index == state.trackIndex ? '*' : ' ');
    oled.print(fileTitle(source.path(index)).substring(0, 19));
  }
  oled.display();
  lastDraw = millis();
}

void updateDisplay() {
  if (!oledReady) return;
  if (screen == Screen::Player) {
    const UiSnapshot state = getUiState();
    if (lastUiTitle != state.title) {
      lastUiTitle = state.title;
      titlePage = 0;
      lastPageChange = millis();
      displayDirty = true;
    }
    const unsigned int pages = (strlen(state.title) + 41) / 42;
    if (pages > 1 && millis() - lastPageChange >= 2500) {
      titlePage = (titlePage + 1) % pages;
      lastPageChange = millis();
      displayDirty = true;
    }
  }
  if (displayDirty && millis() - lastDraw >= 100) {
    displayDirty = false;
    if (screen == Screen::Songs) drawSongMenu();
    else drawPlayer();
  }
}

void singleEncoderClick() {
  if (screen == Screen::Songs) {
    if (sendControl(ControlType::PlayIndex, menuSelection)) screen = Screen::Player;
  } else {
    sendControl(ControlType::TogglePause);
  }
  displayDirty = true;
}

void doubleEncoderClick() {
  if (screen == Screen::Player) {
    menuSelection = constrain(getUiState().trackIndex, 0, mp3Count - 1);
    screen = Screen::Songs;
    Serial.println("Menu de canciones");
  } else {
    screen = Screen::Player;
    Serial.println("Reproductor");
  }
  displayDirty = true;
}

void pollEncoder() {
  const unsigned long now = millis();
  if (!encoderAvailable) {
    if (now - lastEncoderProbe >= kEncoderRetryMs) {
      lastEncoderProbe = now;
      detectEncoder();
    }
    return;
  }
  if (now - lastEncoderPoll < kEncoderPollMs) return;
  lastEncoderPoll = now;

  uint8_t bytes[4];
  uint8_t button;
  if (!readEncoderCommand(kEncoderPositionCommand, bytes, sizeof(bytes)) ||
      !readEncoderCommand(kEncoderButtonCommand, &button, 1)) {
    if (++encoderFailures >= 3) {
      encoderAvailable = false;
      encoderSynced = false;
      lastEncoderProbe = now;
      Serial.println("Encoder desconectado; el reproductor continua");
    }
    return;
  }
  encoderFailures = 0;

  const uint32_t raw = (uint32_t)bytes[0] |
                       ((uint32_t)bytes[1] << 8) |
                       ((uint32_t)bytes[2] << 16) |
                       ((uint32_t)bytes[3] << 24);
  const int32_t position = (int32_t)raw;
  const bool pressed = button != 0;
  if (!encoderSynced) {
    lastEncoderPosition = position;
    lastEncoderButton = pressed;
    buttonTurned = false;
    clickPending = false;
    encoderSynced = true;
    return;
  }

  const int64_t delta = (int64_t)position - lastEncoderPosition;
  lastEncoderPosition = position;
  if (pressed && !lastEncoderButton) buttonTurned = false;
  // Un salto grande suele indicar que el encoder se reinicio.
  if (delta >= -20 && delta <= 20 && delta != 0) {
    clickPending = false;
    if (pressed) {
      buttonTurned = true;
    } else if (screen == Screen::Songs) {
      const int64_t selected = (int64_t)menuSelection + delta;
      menuSelection = (int32_t)constrain(selected, (int64_t)0,
                                         (int64_t)mp3Count - 1);
      displayDirty = true;
    } else {
      // En la pantalla principal, girar cambia a la siguiente/anterior pista.
      const int32_t next = (int32_t)(((int64_t)getUiState().trackIndex + delta + mp3Count) % mp3Count);
      sendControl(ControlType::PlayIndex, next);
    }
  }
  if (!pressed && lastEncoderButton && !buttonTurned) {
    if (clickPending && now - firstClickAt <= kDoubleClickMs) {
      clickPending = false;
      doubleEncoderClick();
    } else {
      clickPending = true;
      firstClickAt = now;
    }
  }
  lastEncoderButton = pressed;
  if (clickPending && now - firstClickAt > kDoubleClickMs) {
    clickPending = false;
    singleEncoderClick();
  }
}

void printHelp() {
  Serial.println("Comandos: v0.0-v1.0 volumen, p pausa, r reanuda,");
  Serial.println("          n siguiente, b anterior, m mute, u unmute,");
  Serial.println("          d diagnostico audio/I2C, f lista MP3, ? ayuda");
  Serial.println("Pruebas: e0/e1 encoder, s0/s1 slider completo,");
  Serial.println("         l0/l1 barra LED, o0/o1 refresco OLED");
  Serial.println("Encoder: girar pista/lista, clic pausa/seleccion,");
  Serial.println("         doble clic abre/cierra lista");
  Serial.println("Slider WS12xx: volumen 0-100%; tras 2.5 s, LED disco al ritmo del audio");
}

void processCommand(const char *command) {
  if (command[1] == '0' || command[1] == '1') {
    if (command[2] == '\0') {
      const bool enabled = command[1] == '1';
      const char *name = nullptr;
      if (command[0] == 'e') {
        encoderPollingEnabled.store(enabled);
        name = "Encoder";
      } else if (command[0] == 's') {
        sliderPollingEnabled.store(enabled);
        name = "Slider";
      } else if (command[0] == 'l') {
        sliderLedsEnabled.store(enabled);
        name = "Barra LED";
      } else if (command[0] == 'o') {
        oledRefreshEnabled.store(enabled);
        if (enabled) displayDirty = true;
        name = "OLED";
      }
      if (name != nullptr) {
        Serial.printf("%s: %s\n", name, enabled ? "activo" : "detenido");
        return;
      }
    }
  }
  if (command[0] == 'v') {
    char *end = nullptr;
    float volume = strtof(command + 1, &end);
    if (end == command + 1 || *end != '\0' || volume < 0.0f || volume > 1.0f) {
      Serial.println("Volumen invalido. Usa v0.0 a v1.0");
      return;
    }
    currentVolume = volume;
    isMuted = false;
    targetGain.store(currentVolume);
    lastVolumeActivityMs.store(millis(), std::memory_order_relaxed);
    Serial.printf("Volumen: %d%%\n", (int)(volume * 100.0f + 0.5f));
  } else if (strcmp(command, "p") == 0) {
    isPaused = true;
    Serial.println("Pausa");
  } else if (strcmp(command, "r") == 0) {
    isPaused = false;
    Serial.println("Reproduciendo");
  } else if (strcmp(command, "n") == 0) {
    if (!player.next()) player.setIndex(0);
    syncTrack();
  } else if (strcmp(command, "b") == 0) {
    if (!player.previous()) player.setIndex(mp3Count - 1);
    syncTrack();
  } else if (strcmp(command, "m") == 0) {
    isMuted = true;
    targetGain.store(0.0f);
    Serial.println("Mute");
  } else if (strcmp(command, "u") == 0) {
    isMuted = false;
    targetGain.store(currentVolume);
    Serial.println("Unmute");
  } else if (strcmp(command, "d") == 0) {
    Serial.printf("I2C0=%lu Hz, I2C1=%lu Hz\n",
                  (unsigned long)myWire.getClock(),
                  (unsigned long)controlWire.getClock());
    Serial.printf("Audio: llamadas=%lu, sin datos=%lu, espera maxima=%lu us, copia maxima=%lu us\n",
                  (unsigned long)audioCopyCalls,
                  (unsigned long)audioEmptyCopies,
                  (unsigned long)maxAudioLoopGapUs,
                  (unsigned long)maxAudioCopyTimeUs);
    Serial.printf("Consultas: encoder=%d, slider=%d, LED=%d, OLED=%d\n",
                  (int)encoderPollingEnabled.load(),
                  (int)sliderPollingEnabled.load(),
                  (int)sliderLedsEnabled.load(),
                  (int)oledRefreshEnabled.load());
    audioCopyCalls = 0;
    audioEmptyCopies = 0;
    maxAudioLoopGapUs = 0;
    maxAudioCopyTimeUs = 0;
    lastAudioCopyEndUs = 0;
    return;
  } else if (strcmp(command, "f") == 0) {
    listMp3Files();
    return;
  } else if (strcmp(command, "?") == 0) {
    printHelp();
  } else if (command[0] != '\0') {
    Serial.println("Comando desconocido; usa ? para ayuda");
  }
  publishUiState();
}

void handleControl() {
  ControlCommand command;
  if (controlQueue == nullptr || xQueueReceive(controlQueue, &command, 0) != pdTRUE) return;
  switch (command.type) {
    case ControlType::PlayIndex:
      if (command.value >= 0 && command.value < mp3Count &&
          player.setIndex(command.value)) {
        isPaused = false;
        syncTrack();
      } else {
        Serial.println("No se pudo abrir la cancion");
      }
      break;
    case ControlType::TogglePause:
      isPaused = !isPaused;
      Serial.println(isPaused ? "Pausa" : "Reproduciendo");
      break;
    case ControlType::SetVolume:
      currentVolume = constrain(command.value, 0, 100) / 100.0f;
      isMuted = false;
      targetGain.store(currentVolume);
      break;
  }
  publishUiState();
}

void uiTask(void *) {
  bool wasEncoderEnabled = true;
  bool wasSliderEnabled = true;
  for (;;) {
    const bool encoderEnabled = encoderPollingEnabled.load();
    if (encoderEnabled) {
      if (!wasEncoderEnabled) {
        encoderSynced = false;
        clickPending = false;
        lastEncoderPoll = millis();
      }
      pollEncoder();
    }
    wasEncoderEnabled = encoderEnabled;

    const bool sliderEnabled = sliderPollingEnabled.load();
    if (sliderEnabled) {
      if (!wasSliderEnabled) {
        sliderReadPending = false;
        sliderNextSampleAt = millis() + 300;
        sliderNextProbeAt = millis() + 300;
      }
      pollSlider();
    }
    wasSliderEnabled = sliderEnabled;
    if (oledRefreshEnabled.load()) updateDisplay();
    vTaskDelay(1);
  }
}

void readSerialCommands() {
  static char command[32];
  static size_t used = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') {
      if (used) {
        command[used] = '\0';
        processCommand(command);
        used = 0;
      }
    } else if (used < sizeof(command) - 1) {
      command[used++] = c;
    }
  }
}

// El driver I2C del core 3.x reserva su interrupcion en el nucleo que llama
// a begin(). Si se inicia desde setup() (nucleo 1), cada transaccion del
// encoder, slider y OLED interrumpe al audio; por eso se inician en el nucleo 0.
void beginI2CTask(void *caller) {
  myWire.begin(SDAPIN, SCLPIN);
  myWire.setClock(I2CSPEED);
  controlWire.begin(ENCODER_SDAPIN, ENCODER_SCLPIN);
  controlWire.setClock(I2CSPEED);
  xTaskNotifyGive((TaskHandle_t)caller);
  vTaskDelete(nullptr);
}

void beginI2CBusesOnCore0() {
  if (xTaskCreatePinnedToCore(beginI2CTask, "i2c_init", 4096,
                              xTaskGetCurrentTaskHandle(), 2, nullptr, 0) == pdPASS) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
  } else {
    myWire.begin(SDAPIN, SCLPIN);
    myWire.setClock(I2CSPEED);
    controlWire.begin(ENCODER_SDAPIN, ENCODER_SCLPIN);
    controlWire.setClock(I2CSPEED);
  }
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);
  delay(1000);

  beginI2CBusesOnCore0();
  controlWire.setTimeOut(kI2CTimeoutMs);
  Serial.printf("Controles en I2C1: SDA=%d SCL=%d\n", ENCODER_SDAPIN, ENCODER_SCLPIN);
#if ENCODER_OWN_I2C
  Serial.println("UE0172 y WS12xx en I2C1");
#else
  myWire.setTimeOut(kI2CTimeoutMs);
  Serial.println("UE0172 en I2C0; WS12xx en I2C1");
#endif
  const uint8_t oledAddress = findOledAddress();
  if (oledAddress != 0) {
    oledReady = oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false);
    Serial.printf("OLED SSD1306 en 0x%02X: %s\n", oledAddress,
                  oledReady ? "OK" : "ERROR");
  } else {
    Serial.println("OLED no encontrada en 0x3C ni 0x3D");
  }
  showMessage("Iniciando...");
  lastEncoderProbe = millis();
  bool encoderFound = detectEncoder();
#if ENCODER_OWN_I2C
  if (!encoderFound) {
    // El UE0172 del ejemplo original usa el valor predeterminado (100 kHz).
    controlWire.setClock(100000);
    encoderFound = detectEncoder();
    if (encoderFound) Serial.println("UE0172 responde solo a 100 kHz; I2C1 queda a 100 kHz");
    else controlWire.setClock(I2CSPEED);
  }
#endif
  if (!encoderFound) Serial.println("Encoder opcional no detectado; continuo");
  if (!detectSlider()) {
    sliderNextProbeAt = millis() + kSliderRetryMs;
    Serial.println("Slider opcional no detectado; continuo");
  }

  Serial.printf("Montando microSD con CS=%d...\n", sdChipSelect);
  if (!SD.begin(sdChipSelect) || SD.cardType() == CARD_NONE) {
    showMessage("Error microSD");
    return;
  }
  showMessage("Leyendo microSD...");
  source.begin();
  mp3Count = source.size();
  Serial.printf("Archivos .mp3 encontrados: %ld\n", mp3Count);
  if (mp3Count == 0) {
    showMessage("No hay archivos MP3");
    return;
  }

  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: pines ES8311 invalidos o ya registrados");
    return;
  }
  auto config = i2s.defaultConfig(TX_MODE);
  config.copyFrom(AudioInfo(44100, 2, 16));
  config.sd_active = false;
  if (!i2s.begin(config)) {
    showMessage("Error ES8311/I2S");
    return;
  }
  musicMeter.begin(AudioInfo(44100, 2, 16));
  i2s.setVolume(0.7f);
  i2s.setMute(false);

  player.setMetadataCallback(onMetadata);
  playerReady = player.begin();
  if (!playerReady) {
    showMessage("Error al abrir MP3");
    return;
  }
  // El volumen lo aplica MusicLevelMeter con rampa; el reproductor va a 1.0.
  player.setVolume(1.0f);
  targetGain.store(currentVolume);
  syncTrack();
  controlQueue = xQueueCreate(8, sizeof(ControlCommand));
  publishUiState();
  displayDirty = true;
  drawPlayer();
  printHelp();
  if (controlQueue == nullptr) {
    Serial.println("No se pudo iniciar la tarea OLED/encoder");
    return;
  }
  const BaseType_t taskResult = xTaskCreatePinnedToCore(
      uiTask, "oled_encoder", 8192, nullptr, 1, &uiTaskHandle, 0);
  if (taskResult == pdPASS) {
    Serial.printf("Interfaz en nucleo 0; audio en nucleo %d\n", xPortGetCoreID());
  } else {
    Serial.println("No se pudo iniciar la tarea OLED/encoder");
  }
}

void loop() {
  if (!playerReady) {
    delay(100);
    return;
  }
  if (!isPaused) {
    const uint32_t startedAt = micros();
    if (lastAudioCopyEndUs != 0) {
      const uint32_t gap = startedAt - lastAudioCopyEndUs;
      if (gap > maxAudioLoopGapUs) maxAudioLoopGapUs = gap;
    }
    const size_t copied = player.copy();
    const uint32_t copyTime = micros() - startedAt;
    lastAudioCopyEndUs = micros();
    if (copyTime > maxAudioCopyTimeUs) maxAudioCopyTimeUs = copyTime;
    ++audioCopyCalls;
    if (copied == 0) ++audioEmptyCopies;
  } else {
    lastAudioCopyEndUs = 0;
  }
  handleControl();
  syncTrack();
  readSerialCommands();
  if (isPaused) delay(1);
}
