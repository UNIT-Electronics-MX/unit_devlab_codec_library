// Reproductor MP3 por microSD con ES8311 y OLED SSD1306 128x64.
// ES8311 (0x18), OLED (0x3C/0x3D) y encoder UE0172 (0x34) comparten I2C0.
// La salida INT del encoder genera un flanco ascendente.
// En modo de una tarea, audio e interfaz se atienden desde loop().
#include <stdlib.h>
#include <atomic>
#include <vector>
#include <SPI.h>
#include <SD.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <UnitDevlabCodecPlayback.h>
#if defined(CONFIG_IDF_TARGET_ESP32C6)
#include <Adafruit_NeoPixel.h>
#endif

#if defined(CONFIG_IDF_TARGET_ESP32C6)
// ESP32-C6: I2S BCLK en GPIO3, WS en GPIO15 y DIN del ES8311 en GPIO20.
#define SDAPIN       22
#define SCLPIN       23
#define MCLKPIN       21
#define BCLKPIN       3
#define WSPIN        15
#define DOPIN        20  // ESP32 -> DSDIN del ES8311
constexpr int kSdCsPin = 19;
constexpr int kSdSckPin = 6;
constexpr int kSdMisoPin = 2;
constexpr int kSdMosiPin = 7;
constexpr int kEncoderInterruptPin = 1;
constexpr int kNeoPixelPin = 8;
constexpr int kNeoPixelCount = 1;
Adafruit_NeoPixel neoPixel(kNeoPixelCount, kNeoPixelPin, NEO_GRB + NEO_KHZ800);
#elif defined(CONFIG_IDF_TARGET_ESP32)
// ESP32 clasico: conexiones originales del reproductor.
#define SDAPIN       21
#define SCLPIN       22
#define MCLKPIN       0
#define BCLKPIN       4
#define WSPIN        16
#define DOPIN        15  // ESP32 -> DSDIN del ES8311
constexpr int kSdCsPin = 5;
constexpr int kSdSckPin = 18;
constexpr int kSdMisoPin = 19;
constexpr int kSdMosiPin = 23;
constexpr int kEncoderInterruptPin = 26;
#else
#error "Este ejemplo admite ESP32 y ESP32-C6"
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
              "microSD CS debe usar un GPIO distinto de I2S WS y encoder INT");
static_assert(BCLKPIN != WSPIN && BCLKPIN != DOPIN && WSPIN != DOPIN,
              "I2S BCLK, WS y DIN deben usar GPIO distintos");
constexpr unsigned long kEncoderPollActiveMs = 20;
constexpr unsigned long kEncoderRetryMs = 2000;
constexpr unsigned long kDoubleClickMs = 350;
constexpr uint16_t kI2CTimeoutMs = 5;
// true: una sola tarea para audio e interfaz; false: interfaz en nucleo 0.
constexpr bool kSingleTaskMode = true;
// Evita transferencias OLED periodicas durante la reproduccion.
constexpr bool kScrollTitleDuringPlayback = false;
// 16 x 512 muestras dan ~186 ms de margen a 44.1 kHz estereo.
constexpr int kAudioDmaBufferCount = 16;

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
std::atomic<bool> i2cReady{false};
// Todos los dispositivos trabajan a 100 kHz; no cambiar el reloj en marcha.
Adafruit_SSD1306 oled(128, 64, &myWire, -1, I2CSPEED, I2CSPEED);
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);
AudioSourceSDList source(startFilePath, extension);
MP3DecoderHelix decoder;
// Volumen pedido (0.0-1.0); el filtro se acerca a el muestra a muestra.
std::atomic<float> targetGain{0.8f};
std::atomic<uint32_t> pcmBytesWritten{0};
std::atomic<uint32_t> pcmWriteFailures{0};

// Aplica el volumen con una rampa para evitar cortes al girar el encoder.
class SmoothVolumeStream : public VolumeMeter {
 public:
  using VolumeMeter::write;
  explicit SmoothVolumeStream(AudioStream &output) : VolumeMeter(output) {}

  size_t write(const uint8_t *data, size_t len) override {
    // Suavizado exponencial de ~20 ms para cada cambio de volumen.
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

enum class Screen { Player, Songs };
Screen screen = Screen::Player;

bool oledReady = false;
uint8_t oledI2cAddress = 0;
bool playerReady = false;
bool isPaused = false;
bool isMuted = false;
std::atomic<bool> displayDirty{true};
std::atomic<bool> footerDirty{false};
// Interruptores de diagnostico: se cambian desde Serial en el nucleo de audio
// y los consulta la tarea de interfaz antes de cada acceso I2C.
std::atomic<bool> encoderPollingEnabled{true};
std::atomic<bool> oledRefreshEnabled{true};
// El porcentaje OLED se actualiza en una region pequena del bus compartido.
std::atomic<bool> oledVolumeLiveEnabled{true};
float currentVolume = 0.8f;
int encoderVolumePercent = 80;
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
    Serial.printf("Encoder UE0172 detectado en 0x%02X\n", kEncoderAddress);
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

// Envia solo las columnas modificadas del buffer SSD1306. display() enviaria
// los 1024 bytes de la pantalla y produce el ruido audible al cambiar volumen.
bool flushOledRegion(uint8_t firstColumn, uint8_t lastColumn,
                     uint8_t firstPage, uint8_t lastPage) {
  if (!oledReady || oledI2cAddress == 0) return false;
  myWire.beginTransmission(oledI2cAddress);
  myWire.write((uint8_t)0x00);  // Comandos SSD1306.
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
      myWire.write((uint8_t)0x40);  // Datos de imagen.
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
    oled.print(state.muted ? "MUTE" : (state.paused ? "PAUSA" : "PLAY"));
    if (!flushOledRegion(0, 35, 6, 7)) {
      oledRefreshEnabled.store(false);
      Serial.println("OLED sin respuesta; refresco detenido");
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
      Serial.println("OLED sin respuesta; refresco detenido");
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
  oled.print(state.muted ? "MUTE" : (state.paused ? "PAUSA" : "PLAY"));
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
      if (!detectEncoder()) {
        encoderPollingEnabled.store(false);
        Serial.println("UE0172 no responde; reintentos detenidos. Envia e1 para probar otra vez");
      }
    }
    return;
  }
  const bool interrupted = takeEncoderInterrupt();
  // El flanco coincide con la accion: leer enseguida, sin esperar 20 ms.
  // Mientras el boton sigue presionado, consultar para detectar cuando se suelta.
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
    encoderVolumePercent = getUiState().volumePercent;
    buttonTurned = false;
    clickPending = false;
    encoderSynced = true;
    return;
  }

  const int64_t delta = (int64_t)position - lastEncoderPosition;
  lastEncoderPosition = position;
  if (pressed && !lastEncoderButton) {
    buttonTurned = false;
    encoderVolumePercent = getUiState().volumePercent;
  }
  // Un salto grande suele indicar que el encoder se reinicio.
  if (delta >= -20 && delta <= 20 && delta != 0) {
    clickPending = false;
    if (screen == Screen::Songs && !pressed && !lastEncoderButton) {
      const int64_t selected = (int64_t)menuSelection + delta;
      menuSelection = (int32_t)constrain(selected, (int64_t)0,
                                         (int64_t)mp3Count - 1);
      displayDirty = true;
    } else {
      if (pressed || lastEncoderButton) buttonTurned = true;
      // En el reproductor, girar ajusta volumen sin cambiar de cancion.
      const int nextVolume = (int)constrain(
          (int64_t)encoderVolumePercent + delta * 5, (int64_t)0, (int64_t)100);
      if (nextVolume != encoderVolumePercent &&
          sendControl(ControlType::SetVolume, nextVolume)) {
        encoderVolumePercent = nextVolume;
      }
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
}

void printHelp() {
  Serial.println("Comandos: v0.0-v1.0 volumen, p pausa, r reanuda,");
  Serial.println("          n siguiente, b anterior, m mute, u unmute,");
  Serial.println("          d diagnostico audio/I2C, f lista MP3, ? ayuda");
  Serial.println("Pruebas: e0/e1 encoder, o0/o1 OLED,");
  Serial.println("         h0/h1 volumen OLED en vivo (h1 activo)");
  Serial.println("Encoder: girar volumen; en la lista, girar selecciona pista,");
  Serial.println("         clic pausa/seleccion, doble clic abre/cierra lista");
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
        name = "Volumen OLED en vivo";
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
    Serial.printf("I2C0=%lu Hz (SDA=%d SCL=%d)\n",
                  (unsigned long)myWire.getClock(), SDAPIN, SCLPIN);
    Serial.printf("Encoder INT: GPIO=%d, flancos=%lu\n",
                  kEncoderInterruptPin,
                  (unsigned long)getEncoderInterruptEdges());
    Serial.printf("Audio: llamadas=%lu, sin datos=%lu, sin datos max=%lu ms, espera maxima=%lu us, copia maxima=%lu us\n",
                  (unsigned long)audioCopyCalls,
                  (unsigned long)audioEmptyCopies,
                  (unsigned long)maxAudioZeroMs,
                  (unsigned long)maxAudioLoopGapUs,
                  (unsigned long)maxAudioCopyTimeUs);
    Serial.printf("Errores I2C: encoder=%lu, OLED parcial=%lu\n",
                  (unsigned long)encoderI2cErrors.load(),
                  (unsigned long)oledI2cErrors.load());
    Serial.printf("Salida PCM: bytes=%lu, escrituras fallidas=%lu, pausa=%d, mute=%d, volumen=%d%%\n",
                  (unsigned long)pcmBytesWritten.exchange(0),
                  (unsigned long)pcmWriteFailures.load(),
                  (int)isPaused, (int)isMuted,
                  (int)(targetGain.load() * 100.0f + 0.5f));
    Serial.printf("Consultas: encoder=%d, OLED=%d\n",
                  (int)encoderPollingEnabled.load(),
                  (int)oledRefreshEnabled.load());
    Serial.printf("Volumen OLED en vivo=%d\n", (int)oledVolumeLiveEnabled.load());
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
    Serial.println("Comando desconocido; usa ? para ayuda");
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
  publishUiState(command.type != ControlType::PlayIndex);
}

void beginUi() {
  Serial.printf("Encoder INT en GPIO %d (flanco ascendente)\n",
                kEncoderInterruptPin);
}

void serviceUi() {
  static bool wasEncoderEnabled = true;
  const bool encoderEnabled = encoderPollingEnabled.load();
  if (encoderEnabled) {
    if (!wasEncoderEnabled) {
      encoderSynced = false;
      clickPending = false;
      lastEncoderPoll = millis();
      lastEncoderProbe = millis() - kEncoderRetryMs;
    }
    pollEncoder();
    if (clickPending && millis() - firstClickAt > kDoubleClickMs) {
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

// La tarea temporal instala I2C e INT en nucleo 0 y se elimina antes de reproducir.
// En ESP32 clasico mantiene esas interrupciones fuera del nucleo del audio.
// ESP32-C6 tiene un solo nucleo y comparte las interrupciones con loop().
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
    // No iniciar I2C en el nucleo de audio si falla la tarea de interfaz.
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
    Serial.println("Error al iniciar I2C");
    return;
  }
  Serial.printf("ES8311, OLED y UE0172 en I2C0: SDA=%d SCL=%d, %lu Hz\n",
                SDAPIN, SCLPIN, (unsigned long)I2CSPEED);
  const uint8_t oledAddress = findOledAddress();
  if (oledAddress != 0) {
    oledI2cAddress = oledAddress;
    oledReady = oled.begin(SSD1306_SWITCHCAPVCC, oledAddress, false, false);
    Serial.printf("OLED SSD1306 en 0x%02X: %s\n", oledAddress,
                  oledReady ? "OK" : "ERROR");
  } else {
    Serial.println("OLED no encontrada en 0x3C ni 0x3D");
  }
  showMessage("Iniciando...");
  lastEncoderProbe = millis();
  bool encoderFound = detectEncoder();
  if (!encoderFound) {
    encoderPollingEnabled.store(false);
    Serial.println("Encoder opcional no detectado; envia e1 para reintentar");
  }
  Serial.printf("microSD SPI: CS=%d SCK=%d MISO=%d MOSI=%d\n",
                kSdCsPin, kSdSckPin, kSdMisoPin, kSdMosiPin);
  if (!SPI.begin(kSdSckPin, kSdMisoPin, kSdMosiPin, kSdCsPin) ||
      !SD.begin(kSdCsPin, SPI) || SD.cardType() == CARD_NONE) {
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

  // El driver del codec recibe el bus I2C ya iniciado.
  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED, false};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: pines ES8311 invalidos o ya registrados");
    return;
  }
  auto config = i2s.defaultConfig(TX_MODE);
  config.copyFrom(AudioInfo(44100, 2, 16));
  config.buffer_count = kAudioDmaBufferCount;
  config.sd_active = false;
  if (!i2s.begin(config)) {
    showMessage("Error ES8311/I2S");
    return;
  }
  musicVolume.begin(AudioInfo(44100, 2, 16));
  i2s.setVolume(0.7f);
  i2s.setMute(false);

  player.setMetadataCallback(onMetadata);
  playerReady = player.begin();
  if (!playerReady) {
    showMessage("Error al abrir MP3");
    return;
  }
  // El volumen lo aplica SmoothVolumeStream con rampa; el reproductor va a 1.0.
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
    Serial.println("No se pudo iniciar el control OLED/encoder");
    return;
  }
  if (kSingleTaskMode) {
    beginUi();
    Serial.printf("Audio e interfaz en loop(), nucleo %d\n", xPortGetCoreID());
  } else {
    const BaseType_t taskResult = xTaskCreatePinnedToCore(
        uiTask, "oled_encoder", 8192, nullptr, 1, &uiTaskHandle, 0);
    if (taskResult == pdPASS) {
      Serial.printf("Interfaz en nucleo 0; audio en nucleo %d\n", xPortGetCoreID());
    } else {
      Serial.println("No se pudo iniciar la tarea OLED/encoder");
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
