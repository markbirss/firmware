# nRF54L15-DK — EBYTE E22-900M30S Wiring Guide

Board: **Nordic nRF54L15-DK (PCA10156)**  
Radio: **EBYTE E22-900M30S** (SX1262, 30 dBm, 868/915 MHz)

---

## Conexiones

Usa el conector **J1** del DK. Los pines están marcados en el PCB con el nombre del GPIO (P0.xx / P1.xx).

> **Nota**: P0.05–P0.09 existen en el SoC pero **no están ruteados** a ningún conector
> físico en la PCA10156 (confirmado en esquemático). Todos los pines del E22 van en P1.

| E22-900M30S | DK J1 pin | GPIO | Función                                       |
|-------------|-----------|------|-----------------------------------------------|
| MISO        | P1.04     | 20   | SPIM20 data in (era uart20 TXD, deshabilitado)|
| NSS / CS    | P1.05     | 21   | SPI chip-select (RadioLib GPIO)               |
| DIO1        | P1.06     | 22   | IRQ — interrupción del modem                  |
| BUSY        | P1.07     | 23   | Señal BUSY (GPIO input)                       |
| NRESET      | P1.08     | 24   | Reset del módulo (GPIO output) (era BTN2)     |
| RXEN        | P1.09     | 25   | LNA enable — held HIGH via ANT_SW (era BTN1)  |
| MOSI        | P1.11     | 27   | SPIM20 data out                               |
| SCK         | P1.12     | 28   | SPIM20 clock                                  |
| GND         | GND (J1)  | —    | Masa común                                    |
| VCC         | VDD (J1)  | —    | 3.3 V                                         |

> **Convención de numeración**: P0.n = n, P1.n = 16+n, P2.n = 32+n  
> Ejemplo: P1.12 → 16+12 = 28

---

## Bridge DIO2 → TXEN (obligatorio)

El módulo E22-900M30S **no conecta DIO2 a TXEN internamente**. Es necesario hacer un puente físico en el módulo:

1. Localizar los pads `DIO2` y `TXEN` en la parte inferior del módulo E22.
2. Soldar un puente de cable o resistencia 0Ω entre ambos pads.
3. Con este puente, el SX1262 controla el PA (Power Amplifier) automáticamente mediante `SX126X_DIO2_AS_RF_SWITCH`.

Sin este puente el módulo **no transmitirá** (PA nunca habilitado).

---

## RXEN — LNA siempre activo

`RXEN` (P0.05) se mantiene en HIGH permanentemente mediante `SX126X_ANT_SW 5` en `variant.h`.  
**No usar** `SX126X_RXEN` — RadioLib lo pondría en LOW en estado IDLE y el LNA quedaría desactivado (radio sordo en RX).

---

## Pines reservados del DK — no conectar

| Pines        | Función reservada                                 |
|--------------|---------------------------------------------------|
| P0.00–P0.03  | UART debug IMCU (uart30, J-Link)                  |
| P0.04        | BTN3                                              |
| P0.05–P0.09  | No ruteados al conector (sin usar)                |
| P1.00–P1.01  | Cristal 32 kHz                                    |
| P1.02–P1.03  | Antena NFC                                        |
| P1.10        | LED1 + pwm20 activo — no reutilizar               |
| P1.13        | BTN0 — único botón de usuario restante            |
| P1.14        | LED3                                              |
| P2.00–P2.05  | Flash QSPI interno                                |
| P2.06–P2.10  | Trace ETM / LED0 / LED2                           |

**Pines reutilizados para E22** (liberados del DK):

| Pines       | Función original | Nuevo uso         |
|-------------|------------------|-------------------|
| P1.04–P1.07 | uart20 (UART1)   | MISO / CS / DIO1 / BUSY |
| P1.08       | BTN2             | NRESET            |
| P1.09       | BTN1             | RXEN (ANT_SW)     |

---

## Build y flash

```bash
# Compilar
~/.platformio/penv/Scripts/pio run -e nrf54l15dk

# Flashear (requiere J-Link conectado)
export PATH="/c/Program Files/SEGGER/JLink_V876:$PATH"
~/.platformio/penv/Scripts/pio run -e nrf54l15dk -t upload

# Monitor RTT (channel 1 = logs de Meshtastic)
"/c/Program Files/SEGGER/JLink_V876/JLinkRTTLogger.exe" \
    -device nRF54L15_M33 -if SWD -speed 4000 \
    -RTTChannel 1 boot.log
```

En el log de boot se debe ver:

```
*** Booting Zephyr OS build zephyr-v40201 ***
[nrf54l15] Reset cause: ...
[nrf54l15] B: calling setup()
INFO  | ... SX1262
INFO  | ... lora.begin() = 0          ← RADIOLIB_ERR_NONE
[nrf54l15] C: setup() returned
```

Si se ve `Record critical error 3` (NO_RADIO) verificar: puente DIO2→TXEN, voltajes, y continuidad del cableado SPI.
