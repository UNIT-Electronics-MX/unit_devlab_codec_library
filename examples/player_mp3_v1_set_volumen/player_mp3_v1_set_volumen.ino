#include <UnitDevlabCodecPlayback.h>
#if __has_include("AudioTools/Disk/AudioSourceSD.h")
#include "AudioTools/Disk/AudioSourceSD.h"
#else
#include "AudioTools/AudioLibs/AudioSourceSD.h"
#endif

// 📂 SD y formato
const char* startFilePath = "/";
const char* ext = "mp3";
AudioSourceSD source(startFilePath, ext);

// 🔈 Decoder
MP3DecoderHelix decoder;

// 🧠 Pines I2C y I2S
#define SDAPIN     21
#define SCLPIN     22
#define ES8311_ADDR 0x18
#define I2CSPEED   100000

#define MCLKPIN    0   // Usa -1 si no tienes MCLK
#define BCLKPIN    17
#define WSPIN      16
#define DOPIN      4    // DAT_OUT del ESP32 -> DIN del ES8311
#define DIPIN      -1   // No se usa entrada

// 📌 Objetos para el codec
DriverPins my_pins;
AudioBoard audio_board(AudioDriverES8311, my_pins);
I2SCodecStream i2s(audio_board);  // <- este sí usa el codec ES8311

// 🎵 Reproductor
AudioPlayer player(source, i2s, decoder);

// 🔧 I2C
TwoWire myWire = TwoWire(0);

// 🔊 Volumen actual (entre 0.0 y 1.0)
float currentVolume = 0.02;

// 📋 Log de metadatos (opcional)
void printMetaData(MetaDataType type, const char* str, int len) {
  Serial.print("==> ");
  Serial.print(toStr(type));
  Serial.print(": ");
  Serial.println(str);
}

void setup() {
  Serial.begin(115200);
  AudioLogger::instance().begin(Serial, AudioLogger::Info);
  delay(1000);
  Serial.println("Inicializando...");

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
  player.begin();
  player.setVolume(currentVolume); // ⬅️ volumen inicial al 10%

  Serial.println("Listo para reproducir MP3 desde microSD con ES8311");
  Serial.println("Escribe 'v0.5' para subir el volumen a 50%");
}

void loop() {
  // Reproducción continua
  player.copy();

  // Leer del Serial para comandos
  if (Serial.available()) {
    String input = Serial.readStringUntil('\n');
    input.trim();

    // Cambiar volumen: ejemplo v0.3
    if (input.startsWith("v")) {
      float v = input.substring(1).toFloat();
      if (v >= 0.0 && v <= 1.0) {
        currentVolume = v;
        player.setVolume(currentVolume);
        Serial.print("🔊 Volumen actualizado a: ");
        Serial.println(currentVolume);
      } else {
        Serial.println("⚠️ Valor inválido. Usa v0.0 a v1.0 (ej. v0.3)");
      }
    }
  }
}
