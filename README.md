# UNIT DevLab: codec ES8311

Biblioteca Arduino para configurar los pines del codec ES8311 en proyectos de audio de UNIT DevLab. Usa el controlador ES8311 de nuestro fork de Audio Driver. Los ejemplos de reproduccion MP3 tambien usan los forks de Audio Tools y LibHelix. Esta biblioteca no copia la implementacion del controlador ni del decodificador.

## Bibliotecas necesarias

Instala estas tres bibliotecas desde sus repositorios, cada una en una carpeta distinta dentro de tu directorio `libraries` de Arduino:

| Biblioteca Arduino | Repositorio |
|---|---|
| `unit_devlab_fork_audio_driver` | [unit_devlab_fork_arduino_audio_driver](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_driver) |
| `unit_devlab_fork_audio_tools` | [unit_devlab_fork_arduino_audio_tools](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_audio_tools) |
| `unit_devlab_fork_libhelix` | [unit_devlab_fork_arduino_libhelix](https://github.com/UNIT-Electronics-MX/unit_devlab_fork_arduino_libhelix) |

El ejemplo senoidal necesita Audio Driver y Audio Tools. Los ejemplos MP3 necesitan los tres forks. Los ejemplos con OLED requieren ademas Adafruit GFX y Adafruit SSD1306. El ejemplo con NeoPixel requiere Adafruit NeoPixel en ESP32-C6.

Los nombres unicos `unit_devlab_fork_*.h` fuerzan la seleccion de los forks aunque haya copias de las bibliotecas originales instaladas. Hasta que los tres forks se registren en Arduino Library Manager, instala sus versiones desde GitHub; por eso `library.properties` no declara dependencias automaticas todavia.

## Uso

```cpp
#include <UnitDevlabCodec.h>

TwoWire codecWire(0);
DriverPins codecPins;
AudioBoard codecBoard(AudioDriverES8311, codecPins);

void setup() {
  const unit_devlab::ES8311Pinout pins{
      21, 22, 0, 4, 16, 15, -1, 0x18, 100000};
  if (!unit_devlab::configureES8311(codecPins, pins, codecWire)) {
    // Pines invalidos o ya registrados.
    return;
  }
  // Inicia codecBoard mediante I2SCodecStream::begin() como en los ejemplos.
}
```

La llamada a `configureES8311` registra los pines, pero no inicializa el codec. `I2SCodecStream::begin()` inicializa I2S y `AudioBoard`; no llames `AudioBoard::begin()` antes del stream. Si el bus I2C ya esta iniciado y lo comparten OLED o controles, fija `i2c_active=false` en `ES8311Pinout` antes de configurarlo.

## Ejemplos

| Sketch | Funcionalidad | Hardware adicional |
|---|---|---|
| `audiotools_custom_max_senoidal` | Genera un tono para probar ES8311 e I2S | Ninguno |
| `player_mp3_v1_set_volumen` | Reproductor MP3 basico con volumen | microSD |
| `player_mp3_uart` | Control de reproduccion por puerto serie | microSD |
| `player_mp3_oled` | Reproductor con pantalla | microSD, OLED, encoder opcional |
| `player_mp3_oled_encoder_volumen` | Pantalla y control de volumen por encoder | microSD, OLED, encoder UE0172 |
| `player_mp3_oled_slider_sin_cortes` | Pantalla y slider en un segundo bus I2C | microSD, OLED, encoder, slider |

Los pines de cada sketch reflejan su prototipo DualMCU y pueden diferir entre variantes ESP32 y ESP32-C6. Comprueba el cableado de tu placa antes de cargarlo. Los seis sketches se verificaron por compilacion con ESP32 Arduino core 3.3.1. Ademas compilaron la variante con encoder para ESP32-C6 y el ejemplo senoidal para ESP32-S3. El audio, los buses compartidos y los controles siguen pendientes de prueba fisica.

Desde el directorio que contiene los cuatro repositorios:

```sh
arduino-cli compile --fqbn esp32:esp32:esp32 \
  --library ./unit_devlab_codec_library \
  --library ./unit_devlab_fork_arduino_audio_driver \
  --library ./unit_devlab_fork_arduino_audio_tools \
  --library ./unit_devlab_fork_arduino_libhelix \
  ./unit_devlab_codec_library/examples/player_mp3_uart
```

## Creditos y licencia

Ejemplos y capa de configuracion: Cesar Bautista / UNIT Electronics. Audio Driver, Audio Tools y la adaptacion de LibHelix son proyectos de Phil Schatzmann; Helix incorpora trabajo de RealNetworks y otros autores. Cada fork conserva su propia licencia. Este repositorio se distribuye bajo GPL-3.0; consulta `LICENSE`.

La revision inicial de este repositorio contenia una copia de `DevLab_BMI323`. Esa revision permanece en el historial de Git; la rama `main` ahora corresponde a ES8311.
