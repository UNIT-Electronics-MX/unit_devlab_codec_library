// Reproductor MP3 por microSD con ES8311 y OLED SSD1306 128x64.
// Variante: slider en nucleo 0 y barra LED con protocolo en buffer si existe.
// ES8311 solo en I2C0: SDA=21, SCL=22.
// OLED, encoder UE0172 (0x31) y slider WS12xx (0x24) en I2C1:
// SDA=25, SCL=26. Mover fisicamente la OLED a esos pines.
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

// 0: encoder junto al ES8311 en 21/22; OLED y slider en 25/26.
// 1: encoder junto a OLED y slider en 25/26 (configuracion normal).
#ifndef ENCODER_OWN_I2C
#define ENCODER_OWN_I2C 1
#endif
#define ENCODER_SDAPIN 25
#define ENCODER_SCLPIN 26
constexpr uint32_t kControlI2CSpeed = 100000;

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
constexpr uint8_t kSliderVersionCommand = 0x01;
constexpr uint8_t kSliderAdcCommand = 0x60;
constexpr uint8_t kSliderPixelCommand = 0xB0;
constexpr uint8_t kSliderPixelBufferedCommand = 0xB9;
constexpr uint8_t kSliderShowCommand = 0xB3;

constexpr uint8_t kSliderClearCommand = 0xB2;
constexpr uint8_t kSliderBrightnessCommand = 0xB5;
constexpr uint8_t kSliderPixelCount = 16;
// Menor corriente de los NeoPixels para reducir ruido en la alimentacion.
constexpr uint8_t kSliderBrightness = 24;
// El ejemplo WS12xx adjunto lee el ADC 2 ms despues del comando.
constexpr unsigned long kSliderReadWaitMs = 2;
constexpr unsigned long kSliderSampleMs = 100;
constexpr unsigned long kSliderRetryMs = 2000;
constexpr unsigned long kVolumeDisplayMs = 2500;
constexpr unsigned long kDiscoFrameMs = 85;
// Evita transferencias OLED periodicas durante la reproduccion.
constexpr bool kScrollTitleDuringPlayback = false;
// 12 x 512 muestras dan ~139 ms de margen a 44.1 kHz estereo.
constexpr int kAudioDmaBufferCount = 12;

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
Adafruit_SSD1306 oled(128, 64, &controlWire, -1,
                     kControlI2CSpeed, kControlI2CSpeed);
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);
AudioSourceSDList source(startFilePath, extension);
MP3DecoderHelix decoder;
std::atomic<uint8_t> musicPeakLevel{0};
// La animacion de audio es opcional; el indicador de volumen sigue activo.
std::atomic<bool> sliderDiscoEnabled{false};

// Volumen pedido (0.0-1.0); MusicLevelMeter se acerca a el muestra a muestra.
std::atomic<float> targetGain{0.8f};

// Aplica el volumen y mide el PCM que realmente sale al codec.
class MusicLevelMeter : public VolumeMeter {
 public:
  using VolumeMeter::write;
  explicit MusicLevelMeter(AudioStream &output) : VolumeMeter(output) {}

  size_t write(const uint8_t *data, size_t len) override {
    // Suavizado exponencial: incluso un cambio pequeno tarda ~20 ms.
    // La rampa anterior completaba un cambio del 2% en ~1 ms.
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
      if (sliderDiscoEnabled.load(std::memory_order_relaxed)) updatePeak();
      done += count;
    }
    if (len > frames * 4) writeAll(data + frames * 4, len - frames * 4);
    return len;
  }

 private:
  float gain = 0.0f;

  void writeAll(const uint8_t *data, size_t len) {
    while (len > 0) {
      // Sin animacion, enviar PCM directo y omitir el calculo del medidor.
      const size_t written = sliderDiscoEnabled.load(std::memory_order_relaxed)
          ? VolumeMeter::write(data, len) : p_out->write(data, len);
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
uint8_t oledI2cAddress = 0;
bool playerReady = false;
bool isPaused = false;
bool isMuted = false;
std::atomic<bool> displayDirty{true};
std::atomic<bool> footerDirty{false};
// Interruptores de diagnostico: se cambian desde Serial en el nucleo de audio
// y los consulta la tarea de interfaz antes de cada acceso I2C.
std::atomic<bool> encoderPollingEnabled{true};
std::atomic<bool> sliderPollingEnabled{true};
// g0 deja activo el I2C del slider y detiene solo sus cambios de ganancia.
std::atomic<bool> sliderVolumeEnabled{true};
// l0 permite aislar las escrituras LED sin perder la lectura ADC del slider.
std::atomic<bool> sliderLedsEnabled{true};
std::atomic<bool> oledRefreshEnabled{true};
// El porcentaje OLED se actualiza en una region pequena del bus de controles.
std::atomic<bool> oledVolumeLiveEnabled{true};
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
bool sliderBufferedPixels = false;
bool sliderPixelsPendingShow = false;
bool sliderDiscoMode = false;
int sliderLastVolumePercent = -1;
int32_t sliderFilteredAdcQ4 = 0;
bool sliderFilterReady = false;
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
    controlWire.beginTransmission(address);
    if (controlWire.endTransmission() == 0) return address;
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
  delay(3);  // Espacio entre comandos cortos de configuracion o Show.
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
  // Ambos comandos aceptan todo el pixel en una sola transaccion I2C.
  controlWire.beginTransmission(kSliderAddress);
  controlWire.write(sliderBufferedPixels ? kSliderPixelBufferedCommand :
                                           kSliderPixelCommand);
  controlWire.write(index);
  controlWire.write(red);
  controlWire.write(green);
  controlWire.write(blue);
  if (controlWire.endTransmission() != 0) return false;
  if (sliderBufferedPixels) {
    sliderPixelsPendingShow = true;
  } else {
    delay(3);  // 0xB0 actualiza el LED inmediatamente.
  }
  return true;
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
  // Una version no legible usa el protocolo antiguo, sin bloquear el audio.
  uint8_t version[2];
  controlWire.beginTransmission(kSliderAddress);
  controlWire.write(kSliderVersionCommand);
  if (controlWire.endTransmission() == 0) {
    delay(5);
    if (controlWire.requestFrom(kSliderAddress, (uint8_t)2) == 2) {
      version[0] = controlWire.read();
      version[1] = controlWire.read();
      sliderBufferedPixels = version[0] > 1U ||
          (version[0] == 1U && version[1] >= 1U);
    } else {
      sliderBufferedPixels = false;
    }
  } else {
    sliderBufferedPixels = false;
  }
  sliderPixelsPendingShow = false;
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
  sliderFilterReady = false;
  sliderNextSampleAt = millis();
  sliderNextVisualAt = millis();
  lastVolumeActivityMs.store(millis(), std::memory_order_relaxed);
  Serial.printf("Slider WS12xx detectado en 0x24; LED %s\n",
                sliderBufferedPixels ? "en buffer" : "modo compatible");
  return true;
}

void disconnectSlider() {
  sliderAvailable = false;
  sliderReadPending = false;
  sliderPixelsPendingShow = false;
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
      if (!detectSlider()) {
        sliderPollingEnabled.store(false);
        Serial.println("WS12xx no responde; reintentos detenidos. Envia s1 para probar otra vez");
      }
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
    // Filtra ruido del ADC sin retrasar los movimientos grandes del slider.
    const int32_t rawQ4 = (int32_t)raw << 4;
    if (raw <= 32U || raw >= 4063U) {
      sliderFilteredAdcQ4 = raw <= 32U ? 0 : 4095 << 4;
      sliderFilterReady = true;
    } else if (!sliderFilterReady) {
      sliderFilteredAdcQ4 = rawQ4;
      sliderFilterReady = true;
    } else {
      const int32_t error = rawQ4 - sliderFilteredAdcQ4;
      sliderFilteredAdcQ4 +=
          abs(error) > (256 << 4) ? (error * 3) / 4 : error / 4;
    }
    const uint16_t stableRaw = (uint16_t)((sliderFilteredAdcQ4 + 8) >> 4);
    sliderFailures = 0;
    sliderNextSampleAt = millis() + kSliderSampleMs;
    sliderVolumeLevel = stableRaw == 0 ? 0 :
        ((uint32_t)stableRaw * kSliderPixelCount + 4094U) / 4095U;
    const int percent = ((uint32_t)stableRaw * 100U + 2047U) / 4095U;
    if (sliderLastVolumePercent < 0 ||
        abs(percent - sliderLastVolumePercent) >= 2 ||
        (percent == 0 && sliderLastVolumePercent != 0) ||
        (percent == 100 && sliderLastVolumePercent != 100)) {
      lastVolumeActivityMs.store(millis(), std::memory_order_relaxed);
      if (sliderVolumeEnabled.load(std::memory_order_relaxed) &&
          sendControl(ControlType::SetVolume, percent)) {
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

  // El firmware nuevo agrupa hasta cuatro pixeles y los muestra una vez.
  // El anterior mantiene un pixel por vuelta para atender el encoder.
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
  const bool disco = sliderDiscoEnabled.load(std::memory_order_relaxed) &&
                     now - lastVolumeActivityMs.load(std::memory_order_relaxed) >=
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
  const uint8_t maxPixels = sliderBufferedPixels ? 4U : 1U;
  for (uint8_t i = 0; i < maxPixels; ++i) {
    if (sliderDisplayedLevel < sliderTargetLevel) {
      if (!setSliderPixel(sliderDisplayedLevel, true, disco)) {
        disconnectSlider();
        return;
      }
      ++sliderDisplayedLevel;
    } else if (sliderDisplayedLevel > sliderTargetLevel) {
      if (!setSliderPixel(sliderDisplayedLevel - 1U, false, disco)) {
        disconnectSlider();
        return;
      }
      --sliderDisplayedLevel;
    } else {
      break;
    }
  }
  if (sliderPixelsPendingShow) {
    if (!writeSliderByte(kSliderShowCommand)) {
      disconnectSlider();
      return;
    }
    sliderPixelsPendingShow = false;
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

// Envia solo las columnas modificadas del buffer SSD1306. display() enviaria
// los 1024 bytes de la pantalla y produce el ruido audible al cambiar volumen.
bool flushOledRegion(uint8_t firstColumn, uint8_t lastColumn,
                     uint8_t firstPage, uint8_t lastPage) {
  if (!oledReady || oledI2cAddress == 0) return false;
  controlWire.beginTransmission(oledI2cAddress);
  controlWire.write((uint8_t)0x00);  // Comandos SSD1306.
  controlWire.write((uint8_t)SSD1306_COLUMNADDR);
  controlWire.write(firstColumn);
  controlWire.write(lastColumn);
  controlWire.write((uint8_t)SSD1306_PAGEADDR);
  controlWire.write(firstPage);
  controlWire.write(lastPage);
  if (controlWire.endTransmission() != 0) return false;

  const uint8_t *buffer = oled.getBuffer();
  for (uint8_t page = firstPage; page <= lastPage; ++page) {
    uint8_t column = firstColumn;
    while (column <= lastColumn) {
      controlWire.beginTransmission(oledI2cAddress);
      controlWire.write((uint8_t)0x40);  // Datos de imagen.
      uint8_t sent = 0;
      while (column <= lastColumn && sent < 28) {
        controlWire.write(buffer[page * 128U + column]);
        ++column;
        ++sent;
      }
      if (controlWire.endTransmission() != 0) return false;
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
  Serial.println("Pruebas: e0/e1 encoder, s0/s1 slider I2C, g0/g1 volumen slider,");
  Serial.println("         l0/l1 barra LED, a0/a1 animacion, o0/o1 OLED,");
  Serial.println("         h0/h1 volumen OLED en vivo (h1 activo)");
  Serial.println("Encoder: girar pista/lista, clic pausa/seleccion,");
  Serial.println("         doble clic abre/cierra lista");
  Serial.println("Slider WS12xx: volumen 0-100%; barra LED fija y brillo bajo");
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
      } else if (command[0] == 'g') {
        sliderVolumeEnabled.store(enabled);
        name = "Volumen del slider";
      } else if (command[0] == 'l') {
        sliderLedsEnabled.store(enabled);
        name = "Barra LED";
      } else if (command[0] == 'a') {
        sliderDiscoEnabled.store(enabled);
        name = "Animacion LED";
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
    Serial.printf("Animacion LED=%d\n", (int)sliderDiscoEnabled.load());
    Serial.printf("Volumen por slider=%d\n", (int)sliderVolumeEnabled.load());
    Serial.printf("Volumen OLED en vivo=%d\n", (int)oledVolumeLiveEnabled.load());
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
        lastEncoderProbe = millis() - kEncoderRetryMs;
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
  controlWire.setClock(kControlI2CSpeed);
  xTaskNotifyGive((TaskHandle_t)caller);
  vTaskDelete(nullptr);
}

bool beginI2CBusesOnCore0() {
  if (xTaskCreatePinnedToCore(beginI2CTask, "i2c_init", 4096,
                              xTaskGetCurrentTaskHandle(), 2, nullptr, 0) == pdPASS) {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    return true;
  } else {
    // No iniciar I2C en el nucleo de audio si falla la tarea de interfaz.
    return false;
  }
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Warning);
  AudioDriverLogger.begin(Serial, AudioDriverLogLevel::Warning);
  delay(1000);

  if (!beginI2CBusesOnCore0()) {
    Serial.println("Error al iniciar I2C en nucleo 0");
    return;
  }
  controlWire.setTimeOut(kI2CTimeoutMs);
  Serial.printf("OLED y controles en I2C1: SDA=%d SCL=%d, %lu Hz\n",
                ENCODER_SDAPIN, ENCODER_SCLPIN,
                (unsigned long)kControlI2CSpeed);
#if ENCODER_OWN_I2C
  Serial.println("OLED, UE0172 y WS12xx en I2C1");
#else
  myWire.setTimeOut(kI2CTimeoutMs);
  Serial.println("UE0172 en I2C0; OLED y WS12xx en I2C1");
#endif
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
  if (!detectSlider()) {
    sliderPollingEnabled.store(false);
    Serial.println("Slider opcional no detectado; envia s1 para reintentar");
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
  config.buffer_count = kAudioDmaBufferCount;
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
  displayDirty = false;
  footerDirty = false;
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
