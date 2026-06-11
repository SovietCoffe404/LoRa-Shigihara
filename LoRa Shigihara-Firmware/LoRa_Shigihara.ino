/*
 * =============================================
 *   LoRa Shigihara — Firmware v1.0
 *   ESP32-S3-WROOM-1 + SX1262
 *   Librería: RadioLib (https://github.com/jgromes/RadioLib)
 * =============================================
 *
 *  Mapeo de pines (extraído del esquemático KiCad):
 *   SX1262 NSS   → IO6   (SPI Chip Select)
 *   SX1262 DIO1  → IO5   (Interrupción IRQ)
 *   SX1262 RESET → IO11
 *   SX1262 BUSY  → IO10
 *   SPI SCK      → IO9
 *   SPI MOSI     → IO7
 *   SPI MISO     → IO8
 *
 *  Instalación de RadioLib en Arduino IDE:
 *   Herramientas → Administrar Bibliotecas → buscar "RadioLib" → Instalar
 *
 *  Placa: ESP32S3 Dev Module (o ESP32-S3-WROOM-1)
 *  Banda: 915 MHz (IFETEL / Américas)
 * =============================================
 */

#include <SPI.h>
#include <RadioLib.h>

// ─── Pines del SX1262 ───────────────────────────────────────────────────────
#define LORA_NSS    6    // Chip Select
#define LORA_DIO1   5    // IRQ (interrupción)
#define LORA_NRST   11   // Reset
#define LORA_BUSY   10   // Busy

// ─── Pines SPI ──────────────────────────────────────────────────────────────
#define SPI_SCK     9
#define SPI_MOSI    7
#define SPI_MISO    8

// ─── Parámetros LoRa ────────────────────────────────────────────────────────
#define LORA_FREQ   915.0   // MHz  — Banda ISM 915 (Américas / IFETEL)
#define LORA_BW     125.0   // kHz  — Ancho de banda
#define LORA_SF     7       // SF7   — Factor de dispersión (7–12)
#define LORA_CR     5       // 4/5  — Tasa de codificación
#define LORA_SYNC   0x34    // Byte de sincronía (LoRaWAN=0x34, privado=cualquier valor)
#define LORA_PWR    14      // dBm  — Potencia de salida (hasta 22 dBm con SX1262)
#define LORA_PREAMBLE 8     // Símbolos de preámbulo

// ─── Instancia del módulo ───────────────────────────────────────────────────
SX1262 radio = new Module(LORA_NSS, LORA_DIO1, LORA_NRST, LORA_BUSY);

// ─── Variables globales ─────────────────────────────────────────────────────
volatile bool txFlag = false;   // Señal: transmisión completa
volatile bool rxFlag = false;   // Señal: paquete recibido
uint32_t pktCounter = 0;        // Contador de paquetes enviados
bool listenMode = false;        // ¿Está el radio en modo RX?

// ─── ISR: DIO1 (llamado por RadioLib en TX/RX done) ─────────────────────────
ICACHE_RAM_ATTR void onRadioEvent() {
  if (listenMode) {
    rxFlag = true;
  } else {
    txFlag = true;
  }
}

// ════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n==============================");
  Serial.println("  LoRa Shigihara — v1.0");
  Serial.println("  ESP32-S3 + SX1262 @ 915 MHz");
  Serial.println("==============================\n");

  // Inicializar SPI con pines personalizados
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI, LORA_NSS);

  // Inicializar SX1262 con RadioLib
  Serial.print("[Radio] Inicializando... ");
  int state = radio.begin(
    LORA_FREQ,
    LORA_BW,
    LORA_SF,
    LORA_CR,
    LORA_SYNC,
    LORA_PWR,
    LORA_PREAMBLE
  );

  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("Error: %d\n", state);
    Serial.println("Revisa las conexiones SPI y alimentación 3.3V.");
    while (true) delay(1000);  // Detener si no hay radio
  }
  Serial.println("OK!");

  // Configurar DIO2 como oscilador RF (necesario para SX1262 con antena U.FL)
  radio.setDio2AsRfSwitch(true);

  // Configurar callback de interrupción
  radio.setDio1Action(onRadioEvent);

  Serial.printf("[Config] Freq=%.1f MHz | BW=%.0f kHz | SF%d | CR4/%d\n\n",
    LORA_FREQ, LORA_BW, LORA_SF, LORA_CR);

  // Comenzar escuchando (RX continuo)
  startRx();
}

// ════════════════════════════════════════════════════════════════════════════
void loop() {
  // ── Recepción ──────────────────────────────────────────────────────────
  if (rxFlag) {
    rxFlag = false;

    String payload;
    int state = radio.readData(payload);

    if (state == RADIOLIB_ERR_NONE) {
      Serial.println("─────────────────────────────");
      Serial.println("  [RX] Paquete recibido!");
      Serial.printf ("  Datos : \"%s\"\n", payload.c_str());
      Serial.printf ("  RSSI  : %.1f dBm\n", radio.getRSSI());
      Serial.printf ("  SNR   : %.1f dB\n",  radio.getSNR());
      Serial.printf ("  Frec  : %.3f MHz\n", radio.getFrequencyError() / 1e6);
      Serial.println("─────────────────────────────\n");
    } else if (state == RADIOLIB_ERR_CRC_MISMATCH) {
      Serial.println("[RX] Error: CRC incorrecto");
    } else {
      Serial.printf("[RX] Error: %d\n", state);
    }

    // Volver a escuchar después de leer
    startRx();
  }

  // ── Transmisión periódica ───────────────────────────────────────────────
  static uint32_t lastTx = 0;
  if (millis() - lastTx >= 3000) {
    lastTx = millis();

    // Construir mensaje
    String msg = "Shigihara #" + String(pktCounter);

    // Detener RX para transmitir
    radio.standby();
    listenMode = false;

    Serial.printf("[TX] Enviando: \"%s\" ... ", msg.c_str());
    int state = radio.startTransmit(msg);

    if (state != RADIOLIB_ERR_NONE) {
      Serial.printf("Error: %d\n", state);
      startRx();
      return;
    }

    // Esperar callback de TX (con timeout)
    uint32_t t = millis();
    while (!txFlag && (millis() - t < 3000)) {
      delay(1);
    }

    if (txFlag) {
      txFlag = false;
      pktCounter++;
      Serial.println("OK");
    } else {
      Serial.println("Timeout!");
    }

    // Volver a modo recepción
    startRx();
  }
}

// ════════════════════════════════════════════════════════════════════════════
// Helpers
// ════════════════════════════════════════════════════════════════════════════

// Poner el radio en escucha continua
void startRx() {
  listenMode = true;
  int state = radio.startReceive();
  if (state != RADIOLIB_ERR_NONE) {
    Serial.printf("[RX] No se pudo iniciar recepción: %d\n", state);
  }
}
