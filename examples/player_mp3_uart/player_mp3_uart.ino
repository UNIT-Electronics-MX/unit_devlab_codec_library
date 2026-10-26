#include <UnitDevlabCodecPlayback.h>
#include <SD.h>
#if __has_include("AudioTools/Disk/AudioSourceSD.h")
#include "AudioTools/Disk/AudioSourceSD.h"
#else
#include "AudioTools/AudioLibs/AudioSourceSD.h"
#endif

// 📂 SD y formato
const char* startFilePath = "/";
const char* ext = ".mp3";
const int sdChipSelect = SS;
AudioSourceSD source(startFilePath, ext, sdChipSelect);

// 🔈 Decoder
MP3DecoderHelix decoder;

// 🧠 Pines I2C y I2S
#define SDAPIN     21
#define SCLPIN     22
#define ES8311_ADDR 0x18
#define I2CSPEED   100000

#define MCLKPIN 0
#define BCLKPIN 4
#define WSPIN 16
#define DOPIN 15   // ESP32 -> DIN del ES8311
#define DIPIN      -1   // No se usa entrada

// 📌 Objetos para el codec
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);  // <- este sí usa el codec ES8311

// 🎵 Reproductor
AudioPlayer player(source, i2s, decoder);

// 🔧 I2C
TwoWire myWire = TwoWire(0);

// 🔊 Estado
float currentVolume = 0.1;
float lastVolume = 0.1;
bool isMuted = false;
bool isPaused = false;
bool playerReady = false;

void listFiles(File directory, int depth = 0) {
  File entry;
  while ((entry = directory.openNextFile())) {
    for (int i = 0; i < depth; ++i) Serial.print("  ");
    Serial.print(entry.name());
    if (entry.isDirectory()) {
      Serial.println("/");
      if (depth < 8) listFiles(entry, depth + 1);
    } else {
      Serial.printf(" (%u bytes)\n", (unsigned int)entry.size());
    }
    entry.close();
  }
}

// 📋 Log de metadatos
void printMetaData(MetaDataType type, const char* str, int len) {
  Serial.print("==> ");
  Serial.print(toStr(type));
  Serial.print(": ");
  Serial.println(str);
}

void printHelp() {
  Serial.println("🎛️ Comandos disponibles:");
  Serial.println("  v0.1 - v1.0  → Cambiar volumen (10% - 100%)");
  Serial.println("  p           → Pausar reproducción");
  Serial.println("  r           → Reanudar reproducción");
  Serial.println("  n           → Siguiente pista");
  Serial.println("  b           → Pista anterior");
  Serial.println("  m           → Mute");
  Serial.println("  u           → Unmute");
  Serial.println("  ?           → Mostrar ayuda");
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Info);
  delay(1000);
  Serial.println("Inicializando...");

  Serial.printf("Montando microSD con CS=%d...\n", sdChipSelect);
  if (!SD.begin(sdChipSelect) || SD.cardType() == CARD_NONE) {
    Serial.println("ERROR: no se pudo montar la microSD. Revisa CS, SPI y alimentacion.");
    return;
  }
  Serial.printf("MicroSD montada: %llu MB\n", SD.cardSize() / (1024ULL * 1024ULL));
  File root = SD.open(startFilePath);
  if (!root || !root.isDirectory()) {
    Serial.println("ERROR: no se pudo abrir el directorio raiz de la microSD.");
    return;
  }
  Serial.println("Archivos en la microSD:");
  listFiles(root);
  root.close();

  source.begin();
  const long mp3Count = source.size();
  Serial.printf("Archivos .mp3 encontrados: %ld\n", mp3Count);
  if (mp3Count == 0) {
    Serial.println("ERROR: no hay archivos .mp3 visibles desde la raiz de la microSD.");
    return;
  }

  // Configurar pines I2C y I2S para codec
  const unit_devlab::ES8311Pinout codecPins{
      SDAPIN, SCLPIN, MCLKPIN, BCLKPIN, WSPIN, DOPIN, DIPIN,
      ES8311_ADDR, I2CSPEED};
  if (!unit_devlab::configureES8311(my_pins, codecPins, myWire)) {
    Serial.println("ERROR: pines ES8311 invalidos o ya registrados");
    return;
  }
  // I2SCodecStream inicializa los pines y el codec ES8311.
  auto cfg = i2s.defaultConfig(TX_MODE);
  if (!i2s.begin(cfg)) {
    Serial.println("ERROR: no se pudo iniciar ES8311/I2S");
    return;
  }

  // Configurar y arrancar el reproductor
  player.setMetadataCallback(printMetaData);
  playerReady = player.begin();
  if (!playerReady) {
    Serial.println("ERROR: no se pudo abrir el primer MP3.");
    return;
  }
  player.setVolume(currentVolume); // Volumen inicial al 10%

  Serial.println("✅ Reproducción lista.");
  printHelp();
}

void loop() {
  if (playerReady && !isPaused) player.copy();

  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    if (input.startsWith("v")) {
      float v = input.substring(1).toFloat();
      if (v >= 0.0 && v <= 1.0) {
        currentVolume = v;
        isMuted = false;
        player.setVolume(currentVolume);
        Serial.print("🔊 Volumen: ");
        Serial.println(currentVolume);
      } else {
        Serial.println("⚠️ Valor inválido. Usa v0.0 a v1.0");
      }

    } else if (input == "p") {
      isPaused = true;
      Serial.println("⏸ Reproducción pausada");

    } else if (input == "r") {
      isPaused = false;
      Serial.println("▶️ Reproducción reanudada");

    } else if (input == "n") {
      player.next();
      Serial.println("⏭ Siguiente pista");

    } else if (input == "b") {
      player.previous();
      Serial.println("⏮ Pista anterior");

    } else if (input == "m") {
      if (!isMuted) {
        lastVolume = currentVolume;
        player.setVolume(0.0);
        isMuted = true;
        Serial.println("🔇 Mute");
      }

    } else if (input == "u") {
      if (isMuted) {
        player.setVolume(lastVolume);
        isMuted = false;
        Serial.println("🔊 Unmute");
      }

    } else if (input == "?") {
      printHelp();

    } else {
      Serial.println("❓ Comando no reconocido. Usa '?' para ver opciones.");
    }
  }
}
