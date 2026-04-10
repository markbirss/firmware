# nRF54L15-DK — EBYTE E22-900M30S Wiring Guide

Board: **Nordic nRF54L15-DK (PCA10156)**  
Radio: **EBYTE E22-900M30S** (SX1262, 30 dBm, 868/915 MHz)

---

## Conexiones

Usa el conector **J1** del DK. Los pines están marcados en el PCB con el nombre del GPIO (P0.xx / P1.xx).

| E22-900M30S | DK J1 pin | GPIO   | Función                          |
|-------------|-----------|--------|----------------------------------|
| NSS / CS    | P0.09     | 9      | SPI chip-select (RadioLib)       |
| SCK         | P1.12     | 28     | SPIM20 clock                     |
| MOSI        | P1.11     | 27     | SPIM20 data out                  |
| MISO        | P0.11     | 11     | SPIM20 data in                   |
| DIO1        | P0.08     | 8      | IRQ — interrupción del modem     |
| BUSY        | P0.07     | 7      | Señal BUSY (GPIO input)          |
| NRESET      | P0.06     | 6      | Reset del módulo (GPIO output)   |
| RXEN        | P0.05     | 5      | LNA enable — held HIGH (ANT_SW)  |
| GND         | GND (J1)  | —      | Masa común                       |
| VCC         | VDD (J1)  | —      | 3.3 V                            |

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

| Pines        | Función reservada        |
|--------------|--------------------------|
| P0.00–P0.03  | UART debug IMCU (J-Link) |
| P0.04        | BTN3                     |
| P1.00–P1.01  | Cristal 32 kHz           |
| P1.02–P1.03  | Antena NFC               |
| P1.04–P1.07  | UART1                    |
| P1.08–P1.09  | BTN2, BTN1               |
| P1.10        | LED1                     |
| P1.13        | BTN0                     |
| P1.14        | LED3                     |
| P2.00–P2.05  | Flash QSPI interno       |
| P2.07        | LED2                     |
| P2.09        | LED0                     |

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
