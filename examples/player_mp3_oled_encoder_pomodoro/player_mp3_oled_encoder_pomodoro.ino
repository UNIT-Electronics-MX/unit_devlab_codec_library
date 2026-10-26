// MP3 player and Pomodoro timer with ES8311, OLED, and UE0172 encoder.
// ES8311 (0x18), OLED (0x3C/0x3D), and UE0172 encoder (0x34) share I2C0.
// The encoder INT output triggers on a rising edge.
// In single-task mode, loop() handles both audio and the UI.
#include <stdlib.h>
#include <atomic>
#include <vector>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <UnitDevlabCodecPlayback.h>
#include <DevLab_AudioSource.h>
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#include <Adafruit_NeoPixel.h>
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C6)
// ESP32-C6: I2S BCLK on GPIO3, WS on GPIO15, and ES8311 DIN on GPIO20.
#define SDAPIN       22
#define SCLPIN       23
#define MCLKPIN       21
#define BCLKPIN       3
#define WSPIN        15
#define DOPIN        20  // ESP32 output -> ES8311 DSDIN
constexpr int kSdCsPin = 19;
constexpr int kSdSckPin = 6;
constexpr int kSdMisoPin = 2;
constexpr int kSdMosiPin = 7;
constexpr int kEncoderInterruptPin = 1;
constexpr int kNeoPixelPin = 8;
constexpr int kNeoPixelCount = 1;
Adafruit_NeoPixel neoPixel(kNeoPixelCount, kNeoPixelPin, NEO_GRB + NEO_KHZ800);
#elif defined(CONFIG_IDF_TARGET_ESP32)
// Classic ESP32: original player wiring.
#define SDAPIN       21
#define SCLPIN       22
#define MCLKPIN       0
#define BCLKPIN       4
#define WSPIN        16
#define DOPIN        15  // ESP32 output -> ES8311 DSDIN
constexpr int kSdCsPin = 5;
constexpr int kSdSckPin = 18;
constexpr int kSdMisoPin = 19;
constexpr int kSdMosiPin = 23;
constexpr int kEncoderInterruptPin = 26;
#else
#error "This example supports ESP32 and ESP32-C6"
#endif

#define I2CSPEED     100000
#define ES8311_ADDR  0x18
#define DIPIN        -1

const char *startFilePath = "/";
const char *extension = ".mp3";
constexpr uint8_t kEncoderAddress = 0x34;
constexpr uint8_t kEncoderIdCommand = 0x00;
constexpr uint8_t kEncoderIdHigh = 0x14;
constexpr uint8_t kEncoderIdLow = 0x01;
constexpr uint8_t kEncoderPositionCommand = 0x80;
constexpr uint8_t kEncoderButtonCommand = 0x81;
static_assert(kSdCsPin != WSPIN && kSdCsPin != kEncoderInterruptPin,
              "microSD CS must use a GPIO distinct from I2S WS and encoder INT");
static_assert(BCLKPIN != WSPIN && BCLKPIN != DOPIN && WSPIN != DOPIN,
              "I2S BCLK, WS, and DIN must use distinct GPIOs");
constexpr unsigned long kEncoderPollActiveMs = 20;
constexpr unsigned long kEncoderRetryMs = 2000;
constexpr unsigned long kDoubleClickMs = 350;
constexpr unsigned long kLongPressMs = 800;
constexpr uint8_t kDefaultFocusMinutes = 25;
constexpr uint8_t kShortBreakMinutes = 5;
constexpr uint8_t kLongBreakMinutes = 15;
constexpr uint8_t kFocusSessionsPerCycle = 4;
constexpr uint16_t kI2CTimeoutMs = 5;
// true: one task for audio and UI; false: UI on core 0.
constexpr bool kSingleTaskMode = true;
// Avoid periodic OLED transfers during playback.
constexpr bool kScrollTitleDuringPlayback = false;
// 16 x 512 samples provide about 186 ms of headroom at 44.1 kHz stereo.
constexpr int kAudioDmaBufferCount = 16;

// AudioSourceSD scans the entire microSD each time a track is selected.
// This source indexes the card once and keeps paths in RAM.
class AudioSourceSDList : public AudioSource {
 public:
  AudioSourceSDList(const char *startPath, const char *ext)
      : startPath(startPath), extension(ext) {}

#if DEVLAB_AUDIO_SOURCE_BEGIN_RETURNS_BOOL
  bool begin() override {
#else
  void begin() override {
#endif
    if (paths.empty()) scan(startPath);
    position = 0;
#if DEVLAB_AUDIO_SOURCE_BEGIN_RETURNS_BOOL
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
std::atomic<bool> i2cReady{false};
// All devices run at 100 kHz; do not change the clock while running.
Adafruit_SSD1306 oled(128, 64, &myWire, -1, I2CSPEED, I2CSPEED);
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);
AudioSourceSDList source(startFilePath, extension);
MP3DecoderHelix decoder;
// Requested volume (0.0-1.0); the filter approaches it sample by sample.
std::atomic<float> targetGain{0.8f};
std::atomic<uint32_t> pcmBytesWritten{0};
std::atomic<uint32_t> pcmWriteFailures{0};

// Apply a volume ramp to avoid glitches when turning the encoder.
class SmoothVolumeStream : public VolumeMeter {
 public:
  using VolumeMeter::write;
  explicit SmoothVolumeStream(AudioStream &output) : VolumeMeter(output) {}

  size_t write(const uint8_t *data, size_t len) override {
    // Exponential smoothing over about 20 ms for each volume change.
    static constexpr float kGainAlpha = 1.0f / 882.0f;
    const int16_t *input = reinterpret_cast<const int16_t *>(data);
    const size_t frames = len / 4;
    int16_t buffer[256];
    size_t done = 0;
    while (done < frames) {
      const size_t count = min(frames - done, sizeof(buffer) / 4);
      const float target = targetGain.load(std::memory_order_relaxed);
      for (size_t i = 0; i < count; ++i) {
        gain += (target - gain) * kGainAlpha;
        buffer[2 * i] = (int16_t)(input[2 * (done + i)] * gain);
        buffer[2 * i + 1] = (int16_t)(input[2 * (done + i) + 1] * gain);
      }
      writeAll(reinterpret_cast<const uint8_t *>(buffer), count * 4);
      done += count;
    }
    if (len > frames * 4) writeAll(data + frames * 4, len - frames * 4);
    return len;
  }

 private:
  float gain = 0.0f;

  void writeAll(const uint8_t *data, size_t len) {
    while (len > 0) {
      const size_t written = p_out->write(data, len);
      if (written == 0) {
        pcmWriteFailures.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      pcmBytesWritten.fetch_add(written, std::memory_order_relaxed);
      data += written;
      len -= written;
    }
  }
};

SmoothVolumeStream musicVolume(i2s);
AudioPlayer player(source, musicVolume, decoder);

enum class Screen { Player, Songs, Pomodoro };
Screen screen = Screen::Player;

enum class PomodoroPhase : uint8_t { Focus, ShortBreak, LongBreak };
PomodoroPhase pomodoroPhase = PomodoroPhase::Focus;
uint8_t focusMinutes = kDefaultFocusMinutes;
uint8_t completedFocusSessions = 0;
uint32_t pomodoroRemainingMs = kDefaultFocusMinutes * 60000UL;
uint32_t pomodoroLastTickMs = 0;
uint32_t lastPomodoroSecondShown = UINT32_MAX;
bool pomodoroRunning = false;

bool oledReady = false;
uint8_t oledI2cAddress = 0;
bool playerReady = false;
bool isPaused = false;
bool isMuted = false;
std::atomic<bool> displayDirty{true};
std::atomic<bool> footerDirty{false};
// Diagnostic switches are changed over Serial on the audio core
// and checked by the UI task before each I2C access.
std::atomic<bool> encoderPollingEnabled{true};
std::atomic<bool> oledRefreshEnabled{true};
// Update the OLED percentage in a small region on the shared bus.
std::atomic<bool> oledVolumeLiveEnabled{true};
float currentVolume = 0.8f;
int encoderVolumePercent = 80;
bool encoderAvailable = false;
bool encoderSynced = false;
bool lastEncoderButton = false;
bool buttonTurned = false;
bool longPressHandled = false;
bool clickPending = false;
int32_t lastEncoderPosition = 0;
int32_t menuSelection = 0;
uint8_t encoderFailures = 0;
unsigned long lastEncoderPoll = 0;
unsigned long lastEncoderProbe = 0;
unsigned long firstClickAt = 0;
unsigned long buttonPressedAt = 0;
std::atomic<uint32_t> encoderI2cErrors{0};
std::atomic<uint32_t> oledI2cErrors{0};
portMUX_TYPE encoderInterruptMux = portMUX_INITIALIZER_UNLOCKED;
volatile bool encoderInterruptPending = false;
volatile uint32_t encoderInterruptEdges = 0;
uint32_t lastAudioCopyEndUs = 0;
uint32_t maxAudioLoopGapUs = 0;
uint32_t maxAudioCopyTimeUs = 0;
uint32_t audioCopyCalls = 0;
uint32_t audioEmptyCopies = 0;
uint32_t audioZeroStartedAtMs = 0;
uint32_t maxAudioZeroMs = 0;
long mp3Count = 0;
String currentFile;
String songTitle;
String songArtist;
unsigned int titlePage = 0;
unsigned long lastPageChange = 0;
unsigned long lastDraw = 0;
unsigned long lastFooterDraw = 0;
int lastFooterVolume = -1;
bool lastFooterPaused = false;
bool lastFooterMuted = false;
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

void publishUiState(bool footerOnly = false) {
  portENTER_CRITICAL(&uiStateMux);
  songTitle.toCharArray(uiSnapshot.title, sizeof(uiSnapshot.title));
  songArtist.toCharArray(uiSnapshot.artist, sizeof(uiSnapshot.artist));
  uiSnapshot.trackIndex = source.index();
  uiSnapshot.volumePercent = (int)(currentVolume * 100.0f + 0.5f);
  uiSnapshot.paused = isPaused;
  uiSnapshot.muted = isMuted;
  portEXIT_CRITICAL(&uiStateMux);
  if (footerOnly) footerDirty = true;
  else displayDirty = true;
}

UiSnapshot getUiState() {
  UiSnapshot state;
  portENTER_CRITICAL(&uiStateMux);
  state = uiSnapshot;
  portEXIT_CRITICAL(&uiStateMux);
  return state;
}

uint32_t pomodoroDurationMs() {
  switch (pomodoroPhase) {
    case PomodoroPhase::Focus: return focusMinutes * 60000UL;
    case PomodoroPhase::ShortBreak: return kShortBreakMinutes * 60000UL;
    case PomodoroPhase::LongBreak: return kLongBreakMinutes * 60000UL;
  }
  return focusMinutes * 60000UL;
}

uint32_t pomodoroSecondsLeft() {
  return (pomodoroRemainingMs + 999UL) / 1000UL;
}

void advancePomodoro(uint32_t now) {
  if (pomodoroPhase == PomodoroPhase::Focus) {
    ++completedFocusSessions;
    pomodoroPhase = completedFocusSessions % kFocusSessionsPerCycle == 0
                         ? PomodoroPhase::LongBreak : PomodoroPhase::ShortBreak;
    Serial.println(pomodoroPhase == PomodoroPhase::LongBreak
                       ? "Focus complete: long break started"
                       : "Focus complete: short break started");
  } else {
    pomodoroPhase = PomodoroPhase::Focus;
    Serial.println("Break complete: focus started");
  }
  pomodoroRemainingMs = pomodoroDurationMs();
  pomodoroLastTickMs = now;
  displayDirty = true;
}

void updatePomodoro(uint32_t now) {
  if (!pomodoroRunning) return;
  const uint32_t elapsed = now - pomodoroLastTickMs;
  if (elapsed == 0) return;
  pomodoroLastTickMs = now;
  if (elapsed >= pomodoroRemainingMs) {
    advancePomodoro(now);
  } else {
    pomodoroRemainingMs -= elapsed;
  }
}

void togglePomodoro() {
  pomodoroRunning = !pomodoroRunning;
  pomodoroLastTickMs = millis();
  displayDirty = true;
  Serial.println(pomodoroRunning ? "Pomodoro running" : "Pomodoro paused");
}

void resetPomodoro() {
  pomodoroRunning = false;
  pomodoroRemainingMs = pomodoroDurationMs();
  displayDirty = true;
  Serial.println("Pomodoro phase reset");
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

bool readEncoderCommand(uint8_t command, uint8_t *reply, uint8_t length) {
  myWire.beginTransmission(kEncoderAddress);
  myWire.write(command);
  if (myWire.endTransmission() != 0 ||
      myWire.requestFrom(kEncoderAddress, length) != length) {
    encoderI2cErrors.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  for (uint8_t i = 0; i < length; ++i) reply[i] = myWire.read();
  return true;
}

bool detectEncoder() {
  uint8_t id[2];
  encoderAvailable = readEncoderCommand(kEncoderIdCommand, id, sizeof(id)) &&
                     id[0] == kEncoderIdHigh && id[1] == kEncoderIdLow;
  if (encoderAvailable) {
    encoderSynced = false;
    encoderFailures = 0;
    Serial.printf("UE0172 encoder detected at 0x%02X\n", kEncoderAddress);
  }
  return encoderAvailable;
}

void IRAM_ATTR onEncoderInterrupt() {
  portENTER_CRITICAL_ISR(&encoderInterruptMux);
  encoderInterruptPending = true;
  ++encoderInterruptEdges;
  portEXIT_CRITICAL_ISR(&encoderInterruptMux);
}

bool takeEncoderInterrupt() {
  portENTER_CRITICAL(&encoderInterruptMux);
  const bool pending = encoderInterruptPending;
  encoderInterruptPending = false;
  portEXIT_CRITICAL(&encoderInterruptMux);
  return pending;
}

uint32_t getEncoderInterruptEdges() {
  portENTER_CRITICAL(&encoderInterruptMux);
  const uint32_t edges = encoderInterruptEdges;
  portEXIT_CRITICAL(&encoderInterruptMux);
  return edges;
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
  return result.length() ? result : "Untitled";
}

void syncTrack() {
  const char *path = source.toStr();
  if (path == nullptr) return;
  if (strcmp(path, currentFile.c_str()) == 0) return;
  currentFile = path;
  songTitle = fileTitle(currentFile);
  songArtist = "";
  publishUiState();
  Serial.print("Playing: ");
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

// Send only changed columns from the SSD1306 buffer. display() would send
// all 1024 screen bytes and cause audible noise when changing volume.
bool flushOledRegion(uint8_t firstColumn, uint8_t lastColumn,
                     uint8_t firstPage, uint8_t lastPage) {
  if (!oledReady || oledI2cAddress == 0) return false;
  myWire.beginTransmission(oledI2cAddress);
  myWire.write((uint8_t)0x00);  // SSD1306 commands.
  myWire.write((uint8_t)SSD1306_COLUMNADDR);
  myWire.write(firstColumn);
  myWire.write(lastColumn);
  myWire.write((uint8_t)SSD1306_PAGEADDR);
  myWire.write(firstPage);
  myWire.write(lastPage);
  if (myWire.endTransmission() != 0) {
    oledI2cErrors.fetch_add(1, std::memory_order_relaxed);
    return false;
  }

  const uint8_t *buffer = oled.getBuffer();
  for (uint8_t page = firstPage; page <= lastPage; ++page) {
    uint8_t column = firstColumn;
    while (column <= lastColumn) {
      myWire.beginTransmission(oledI2cAddress);
      myWire.write((uint8_t)0x40);  // Image data.
      uint8_t sent = 0;
      while (column <= lastColumn && sent < 28) {
        myWire.write(buffer[page * 128U + column]);
        ++column;
        ++sent;
      }
      if (myWire.endTransmission() != 0) {
        oledI2cErrors.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
    }
  }
  return true;
}

void drawPlayerFooter() {
  if (!oledReady || screen != Screen::Player) return;
  const UiSnapshot state = getUiState();
  const bool statusChanged = state.paused != lastFooterPaused ||
                             state.muted != lastFooterMuted;
  const bool volumeChanged = oledVolumeLiveEnabled.load(std::memory_order_relaxed) &&
                             state.volumePercent != lastFooterVolume;
  if (!statusChanged && !volumeChanged) return;
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  if (statusChanged) {
    oled.fillRect(0, 48, 36, 16, SSD1306_BLACK);
    oled.setCursor(0, 53);
    oled.print(state.muted ? "MUTE" : (state.paused ? "PAUSE" : "PLAY"));
    if (!flushOledRegion(0, 35, 6, 7)) {
      oledRefreshEnabled.store(false);
      Serial.println("OLED unresponsive; refresh stopped");
      return;
    }
    lastFooterPaused = state.paused;
    lastFooterMuted = state.muted;
  }
  if (volumeChanged) {
    oled.fillRect(72, 48, 30, 16, SSD1306_BLACK);
    oled.setCursor(72, 53);
    oled.print(state.volumePercent);
    oled.print('%');
    if (!flushOledRegion(72, 101, 6, 7)) {
      oledRefreshEnabled.store(false);
      Serial.println("OLED unresponsive; refresh stopped");
      return;
    }
    lastFooterVolume = state.volumePercent;
  }
  lastFooterDraw = millis();
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
  oled.print(state.muted ? "MUTE" : (state.paused ? "PAUSE" : "PLAY"));
  oled.setCursor(42, 53);
  oled.print("Vol:");
  oled.setCursor(72, 53);
  oled.print(state.volumePercent);
  oled.print("%");
  oled.display();
  lastDraw = millis();
  lastFooterDraw = lastDraw;
  lastFooterVolume = state.volumePercent;
  lastFooterPaused = state.paused;
  lastFooterMuted = state.muted;
  lastUiTitle = state.title;
}

void drawSongMenu() {
  if (!oledReady) return;
  const UiSnapshot state = getUiState();
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  oled.print("TRACKS ");
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

void formatPomodoroTime(char (&text)[6]) {
  const uint32_t seconds = pomodoroSecondsLeft();
  snprintf(text, sizeof(text), "%02lu:%02lu",
           (unsigned long)(seconds / 60UL),
           (unsigned long)(seconds % 60UL));
}

void drawPomodoro() {
  if (!oledReady) return;
  oled.clearDisplay();
  oled.setTextSize(1);
  oled.setTextColor(SSD1306_WHITE);
  oled.setCursor(0, 0);
  if (pomodoroPhase == PomodoroPhase::Focus) {
    oled.print("FOCUS ");
    oled.print(completedFocusSessions % kFocusSessionsPerCycle + 1);
    oled.print("/4");
  } else {
    oled.print(pomodoroPhase == PomodoroPhase::ShortBreak
                   ? "SHORT BREAK" : "LONG BREAK");
  }
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  char timeText[6];
  formatPomodoroTime(timeText);
  oled.setTextSize(2);
  oled.setCursor(34, 19);
  oled.print(timeText);
  oled.setTextSize(1);
  oled.setCursor(0, 43);
  oled.print(pomodoroRunning ? "RUNNING 1=PAUSE" : "PAUSED 1=START");
  oled.setCursor(0, 55);
  oled.print("2X RESET HOLD=MP3");
  oled.display();
  lastDraw = millis();
  lastPomodoroSecondShown = pomodoroSecondsLeft();
}

void drawPomodoroTime() {
  if (!oledReady) return;
  const uint32_t seconds = pomodoroSecondsLeft();
  if (seconds == lastPomodoroSecondShown) return;
  oled.fillRect(30, 16, 69, 24, SSD1306_BLACK);
  char timeText[6];
  formatPomodoroTime(timeText);
  oled.setTextSize(2);
  oled.setCursor(34, 19);
  oled.print(timeText);
  oled.setTextSize(1);
  if (!flushOledRegion(30, 98, 2, 4)) {
    oledRefreshEnabled.store(false);
    Serial.println("OLED unresponsive; Pomodoro refresh stopped");
    return;
  }
  lastPomodoroSecondShown = seconds;
  lastDraw = millis();
}

void updateDisplay() {
  if (!oledReady) return;
  if (screen == Screen::Pomodoro) {
    if (displayDirty && millis() - lastDraw >= 100) {
      displayDirty.exchange(false);
      footerDirty.exchange(false);
      drawPomodoro();
    } else if (pomodoroRunning && millis() - lastDraw >= 100) {
      drawPomodoroTime();
    }
    return;
  }
  if (screen == Screen::Player) {
    const UiSnapshot state = getUiState();
    if (lastUiTitle != state.title) {
      lastUiTitle = state.title;
      titlePage = 0;
      lastPageChange = millis();
      displayDirty = true;
    }
    const unsigned int pages = (strlen(state.title) + 41) / 42;
    if (kScrollTitleDuringPlayback && pages > 1 &&
        millis() - lastPageChange >= 2500) {
      titlePage = (titlePage + 1) % pages;
      lastPageChange = millis();
      displayDirty = true;
    }
  }
  if (displayDirty && millis() - lastDraw >= 100) {
    displayDirty.exchange(false);
    footerDirty.exchange(false);
    if (screen == Screen::Songs) drawSongMenu();
    else drawPlayer();
  } else if (footerDirty && millis() - lastFooterDraw >= 100) {
    footerDirty.exchange(false);
    drawPlayerFooter();
  }
}

void singleEncoderClick() {
  if (screen == Screen::Pomodoro) {
    togglePomodoro();
  } else if (screen == Screen::Songs) {
    if (sendControl(ControlType::PlayIndex, menuSelection)) screen = Screen::Player;
  } else {
    sendControl(ControlType::TogglePause);
  }
  displayDirty = true;
}

void doubleEncoderClick() {
  if (screen == Screen::Pomodoro) {
    resetPomodoro();
  } else if (screen == Screen::Player) {
    menuSelection = constrain(getUiState().trackIndex, 0, mp3Count - 1);
    screen = Screen::Songs;
    Serial.println("Track list");
  } else {
    screen = Screen::Player;
    Serial.println("Player");
  }
  displayDirty = true;
}

void longEncoderPress() {
  screen = screen == Screen::Pomodoro ? Screen::Player : Screen::Pomodoro;
  clickPending = false;
  displayDirty = true;
  footerDirty = false;
  Serial.println(screen == Screen::Pomodoro ? "Pomodoro view" : "MP3 player view");
}

void pollEncoder() {
  const unsigned long now = millis();
  if (!encoderAvailable) {
    if (now - lastEncoderProbe >= kEncoderRetryMs) {
      lastEncoderProbe = now;
      if (!detectEncoder()) {
        encoderPollingEnabled.store(false);
        Serial.println("UE0172 unresponsive; retries stopped. Send e1 to retry");
      }
    }
    return;
  }
  const bool interrupted = takeEncoderInterrupt();
  // The edge matches the action: read immediately without waiting 20 ms.
  // Keep polling while the button is pressed to detect release.
  if (encoderSynced && !interrupted &&
      (!lastEncoderButton || now - lastEncoderPoll < kEncoderPollActiveMs)) return;
  lastEncoderPoll = now;

  uint8_t bytes[4];
  uint8_t button;
  if (!readEncoderCommand(kEncoderPositionCommand, bytes, sizeof(bytes)) ||
      !readEncoderCommand(kEncoderButtonCommand, &button, 1)) {
    if (++encoderFailures >= 3) {
      encoderAvailable = false;
      encoderSynced = false;
      lastEncoderProbe = now;
      Serial.println("Encoder disconnected; playback continues");
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
    buttonPressedAt = now;
    encoderVolumePercent = getUiState().volumePercent;
    buttonTurned = false;
    longPressHandled = false;
    clickPending = false;
    encoderSynced = true;
    return;
  }

  const int64_t delta = (int64_t)position - lastEncoderPosition;
  lastEncoderPosition = position;
  if (pressed && !lastEncoderButton) {
    buttonTurned = false;
    longPressHandled = false;
    buttonPressedAt = now;
    encoderVolumePercent = getUiState().volumePercent;
  }
  // A large jump usually means the encoder restarted.
  if (delta >= -20 && delta <= 20 && delta != 0) {
    clickPending = false;
    if (screen == Screen::Songs && !pressed && !lastEncoderButton) {
      const int64_t selected = (int64_t)menuSelection + delta;
      menuSelection = (int32_t)constrain(selected, (int64_t)0,
                                         (int64_t)mp3Count - 1);
      displayDirty = true;
    } else if (screen == Screen::Pomodoro && !pomodoroRunning &&
               pomodoroPhase == PomodoroPhase::Focus &&
               !pressed && !lastEncoderButton) {
      const int nextMinutes = constrain((int)focusMinutes + (int)delta, 1, 60);
      if (nextMinutes != focusMinutes) {
        focusMinutes = nextMinutes;
        pomodoroRemainingMs = pomodoroDurationMs();
        displayDirty = true;
      }
    } else {
      if (pressed || lastEncoderButton) buttonTurned = true;
      // Volume remains available while the timer is running or a break is active.
      const int nextVolume = (int)constrain(
          (int64_t)encoderVolumePercent + delta * 5, (int64_t)0, (int64_t)100);
      if (nextVolume != encoderVolumePercent &&
          sendControl(ControlType::SetVolume, nextVolume)) {
        encoderVolumePercent = nextVolume;
      }
    }
  }
  if (pressed && !buttonTurned && !longPressHandled &&
      now - buttonPressedAt >= kLongPressMs) {
    longPressHandled = true;
    longEncoderPress();
  }
  if (!pressed && lastEncoderButton && !buttonTurned && !longPressHandled) {
    if (clickPending && now - firstClickAt <= kDoubleClickMs) {
      clickPending = false;
      doubleEncoderClick();
    } else {
      clickPending = true;
      firstClickAt = now;
    }
  }
  lastEncoderButton = pressed;
}

void printHelp() {
  Serial.println("Commands: v0.0-v1.0 volume, p pause, r resume,");
  Serial.println("          n next, b previous, m mute, u unmute,");
  Serial.println("          d audio/I2C diagnostics, f MP3 list, ? help");
  Serial.println("Tests: e0/e1 encoder, o0/o1 OLED,");
  Serial.println("       h0/h1 live OLED volume (h1 active)");
  Serial.println("Encoder: turn for volume; in list view, turn to select a track,");
  Serial.println("         click to pause/select; double-click to open/close list");
  Serial.println("Pomodoro: hold to open/close; click start/pause; double-click reset");
  Serial.println("          turn while stopped in focus to set 1-60 minutes");
  Serial.println("          focus 25 min, short break 5 min, long break 15 min after 4");
}

void processCommand(const char *command) {
  if (command[1] == '0' || command[1] == '1') {
    if (command[2] == '\0') {
      const bool enabled = command[1] == '1';
      const char *name = nullptr;
      if (command[0] == 'e') {
        encoderPollingEnabled.store(enabled);
        name = "Encoder";
      } else if (command[0] == 'o') {
        oledRefreshEnabled.store(enabled);
        if (enabled) displayDirty = true;
        name = "OLED";
      } else if (command[0] == 'h') {
        oledVolumeLiveEnabled.store(enabled);
        if (enabled) footerDirty = true;
        name = "Live OLED volume";
      }
      if (name != nullptr) {
        Serial.printf("%s: %s\n", name, enabled ? "active" : "stopped");
        return;
      }
    }
  }
  if (command[0] == 'v') {
    char *end = nullptr;
    float volume = strtof(command + 1, &end);
    if (end == command + 1 || *end != '\0' || volume < 0.0f || volume > 1.0f) {
      Serial.println("Invalid volume. Use v0.0 to v1.0");
      return;
    }
    currentVolume = volume;
    isMuted = false;
    targetGain.store(currentVolume);
    Serial.printf("Volume: %d%%\n", (int)(volume * 100.0f + 0.5f));
  } else if (strcmp(command, "p") == 0) {
    isPaused = true;
    Serial.println("Paused");
  } else if (strcmp(command, "r") == 0) {
    isPaused = false;
    Serial.println("Playing");
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
    Serial.printf("I2C0=%lu Hz (SDA=%d SCL=%d)\n",
                  (unsigned long)myWire.getClock(), SDAPIN, SCLPIN);
    Serial.printf("Encoder INT: GPIO=%d, edges=%lu\n",
                  kEncoderInterruptPin,
                  (unsigned long)getEncoderInterruptEdges());
    Serial.printf("Audio: calls=%lu, no data=%lu, max no data=%lu ms, max wait=%lu us, max copy=%lu us\n",
                  (unsigned long)audioCopyCalls,
                  (unsigned long)audioEmptyCopies,
                  (unsigned long)maxAudioZeroMs,
                  (unsigned long)maxAudioLoopGapUs,
                  (unsigned long)maxAudioCopyTimeUs);
    Serial.printf("I2C errors: encoder=%lu, partial OLED=%lu\n",
                  (unsigned long)encoderI2cErrors.load(),
                  (unsigned long)oledI2cErrors.load());
    Serial.printf("PCM output: bytes=%lu, failed writes=%lu, paused=%d, mute=%d, volume=%d%%\n",
                  (unsigned long)pcmBytesWritten.exchange(0),
                  (unsigned long)pcmWriteFailures.load(),
                  (int)isPaused, (int)isMuted,
                  (int)(targetGain.load() * 100.0f + 0.5f));
    Serial.printf("Polling: encoder=%d, OLED=%d\n",
                  (int)encoderPollingEnabled.load(),
                  (int)oledRefreshEnabled.load());
    Serial.printf("Live OLED volume=%d\n", (int)oledVolumeLiveEnabled.load());
    audioCopyCalls = 0;
    audioEmptyCopies = 0;
    maxAudioZeroMs = 0;
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
    Serial.println("Unknown command; use ? for help");
  }
  const bool footerOnly = command[0] == 'v' || strcmp(command, "p") == 0 ||
                          strcmp(command, "r") == 0 ||
                          strcmp(command, "m") == 0 ||
                          strcmp(command, "u") == 0;
  publishUiState(footerOnly);
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
        Serial.println("Could not open track");
      }
      break;
    case ControlType::TogglePause:
      isPaused = !isPaused;
      Serial.println(isPaused ? "Paused" : "Playing");
      break;
    case ControlType::SetVolume:
      currentVolume = constrain(command.value, 0, 100) / 100.0f;
      isMuted = false;
      targetGain.store(currentVolume);
      break;
  }
  publishUiState(command.type != ControlType::PlayIndex);
}

void beginUi() {
  Serial.printf("Encoder INT on GPIO %d (rising edge)\n",
                kEncoderInterruptPin);
}

void serviceUi() {
  static bool wasEncoderEnabled = true;
  updatePomodoro(millis());
  const bool encoderEnabled = encoderPollingEnabled.load();
  if (encoderEnabled) {
    if (!wasEncoderEnabled) {
      encoderSynced = false;
      clickPending = false;
      lastEncoderPoll = millis();
      lastEncoderProbe = millis() - kEncoderRetryMs;
    }
    pollEncoder();
    if (clickPending && !lastEncoderButton &&
        millis() - firstClickAt > kDoubleClickMs) {
      clickPending = false;
      singleEncoderClick();
    }
  }
  wasEncoderEnabled = encoderEnabled;

  if (oledRefreshEnabled.load()) updateDisplay();
}

void uiTask(void *) {
  beginUi();
  for (;;) {
    serviceUi();
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

// A temporary task sets up I2C and INT on core 0, then exits before playback.
// On classic ESP32, this keeps those interrupts off the audio core.
// ESP32-C6 has one core and shares interrupts with loop().
void beginI2CTask(void *caller) {
  const bool ready = myWire.begin(SDAPIN, SCLPIN, I2CSPEED);
  if (ready) {
    myWire.setTimeOut(kI2CTimeoutMs);
    pinMode(kEncoderInterruptPin, INPUT);
    attachInterrupt(digitalPinToInterrupt(kEncoderInterruptPin),
                    onEncoderInterrupt, RISING);
  }
  i2cReady.store(ready, std::memory_order_release);
  xTaskNotifyGive((TaskHandle_t)caller);
  vTaskDelete(nullptr);
}

bool beginI2COnCore0() {
  if (xTaskCreatePinnedToCore(beginI2CTask, "i2c_init", 4096,
                              xTaskGetCurrentTaskHandle(), 2, nullptr, 0) == pdPASS) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return i2cReady.load(std::memory_order_acquire);
  } else {
    // Do not start I2C on the audio core if the UI task fails.
    return false;
  }
}

void setup() {
#if defined(CONFIG_IDF_TARGET_ESP32C6)
  neoPixel.begin();
  neoPixel.setPixelColor(0, neoPixel.Color(0, 0, 16));
  neoPixel.show();
#endif
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);
  delay(1000);

  if (!beginI2COnCore0()) {
    Serial.println("Could not start I2C");
    return;
  }
  Serial.printf("ES8311, OLED, and UE0172 on I2C0: SDA=%d SCL=%d, %lu Hz\n",
                SDAPIN, SCLPIN, (unsigned long)I2CSPEED);
  const uint8_t oledAddress = findOledAddress();
  if (oledAddress != 0) {
    oledI2cAddress = oledAddress;
    oledReady = oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false);
    Serial.printf("OLED SSD1306 at 0x%02X: %s\n", oledAddress,
                  oledReady ? "OK" : "ERROR");
  } else {
    Serial.println("OLED not found at 0x3C or 0x3D");
  }
  showMessage("Starting...");
  lastEncoderProbe = millis();
  bool encoderFound = detectEncoder();
  if (!encoderFound) {
    encoderPollingEnabled.store(false);
    Serial.println("Optional encoder not found; send e1 to retry");
  }
  Serial.printf("microSD SPI: CS=%d SCK=%d MISO=%d MOSI=%d\n",
                kSdCsPin, kSdSckPin, kSdMisoPin, kSdMosiPin);
  if (!SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin) ||
      !SD.begin(kSdCsPin, SPI) || SD.cardType() == CARD_NONE) {
    showMessage("microSD error");
    return;
  }
  showMessage("Reading microSD...");
  source.begin();
  mp3Count = source.size();
  Serial.printf(".mp3 files found: %ld\n", mp3Count);
  if (mp3Count == 0) {
    showMessage("No MP3 files found");
    return;
  }

  // The codec driver receives an already started I2C bus.
  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED, false};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: invalid ES8311 pins or pins already registered");
    return;
  }
  auto config = i2s.defaultConfig(TX_MODE);
  config.copyFrom(AudioInfo(44100, 2, 16));
  config.buffer_count = kAudioDmaBufferCount;
  config.sd_active = false;
  if (!i2s.begin(config)) {
    showMessage("ES8311/I2S error");
    return;
  }
  musicVolume.begin(AudioInfo(44100, 2, 16));
  i2s.setVolume(0.7f);
  i2s.setMute(false);

  player.setMetadataCallback(onMetadata);
  playerReady = player.begin();
  if (!playerReady) {
    showMessage("Could not open MP3");
    return;
  }
  // SmoothVolumeStream applies a volume ramp; the player stays at 1.0.
  player.setVolume(1.0f);
  targetGain.store(currentVolume);
  syncTrack();
  controlQueue = xQueueCreate(8, sizeof(ControlCommand));
  publishUiState();
  displayDirty = true;
  drawPlayer();
  displayDirty = false;
  footerDirty = false;
  printHelp();
  if (controlQueue == nullptr) {
    Serial.println("Could not start OLED/encoder control");
    return;
  }
  if (kSingleTaskMode) {
    beginUi();
    Serial.printf("Audio and UI in loop(), core %d\n", xPortGetCoreID());
  } else {
    const BaseType_t taskResult = xTaskCreatePinnedToCore(
        uiTask, "oled_encoder", 8192, nullptr, 1, &uiTaskHandle, 0);
    if (taskResult == pdPASS) {
      Serial.printf("UI on core 0; audio on core %d\n", xPortGetCoreID());
    } else {
      Serial.println("Could not start OLED/encoder task");
    }
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
    if (copied == 0) {
      ++audioEmptyCopies;
      const uint32_t nowMs = millis();
      if (audioZeroStartedAtMs == 0) audioZeroStartedAtMs = nowMs;
      const uint32_t zeroDurationMs = nowMs - audioZeroStartedAtMs;
      if (zeroDurationMs > maxAudioZeroMs) maxAudioZeroMs = zeroDurationMs;
    } else {
      audioZeroStartedAtMs = 0;
    }
  } else {
    lastAudioCopyEndUs = 0;
  }
  if (kSingleTaskMode) serviceUi();
  handleControl();
  syncTrack();
  readSerialCommands();
  if (isPaused) delay(1);
}
