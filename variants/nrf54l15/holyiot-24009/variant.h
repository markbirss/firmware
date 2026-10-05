#pragma once

#define VARIANT_MCK (128000000ul)
#define USE_LFXO

#include "WVariant.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PINS_COUNT (96)
#define NUM_DIGITAL_PINS (96)
#define NUM_ANALOG_INPUTS (8)
#define NUM_ANALOG_OUTPUTS (0)
#define ADC_RESOLUTION 14

// LEDs (active low): LED1 P1.10 status, LED0 P2.09
#define PIN_LED1 42
#define PIN_LED2 73
#define LED_BUILTIN PIN_LED1
#define LED_STATE_ON 0

// BTN0 P1.13 (active low)
#define PIN_BUTTON1 45
#define BUTTON_NEED_PULLUP

#define USE_SH1107

// Serial1: VCOM0 of the on-board J-Link (UARTE20): TX P1.04, RX P1.05
#define PIN_SERIAL1_RX 44
#define PIN_SERIAL1_TX 43
#define SERIAL1_UARTE NRF_UARTE20
#define SERIAL1_IRQN SERIAL20_IRQn
#define SERIAL1_IRQ_HANDLER SERIAL20_IRQHandler

// Serial2 (UARTE21, serial module): RX P1.15, TX P1.16; only P1 pins can be assigned to it
#define PIN_SERIAL2_RX 37
#define PIN_SERIAL2_TX 38
#define SERIAL2_UARTE NRF_UARTE21
#define SERIAL2_IRQN SERIAL21_IRQn
#define SERIAL2_IRQ_HANDLER SERIAL21_IRQHandler

// SPI (SPIM00) for the ESX1262
#define SPI_INTERFACES_COUNT 1
#define PIN_SPI_MISO 68
#define PIN_SPI_MOSI 66
#define PIN_SPI_SCK 65
static const uint8_t SS = 69;
static const uint8_t MOSI = PIN_SPI_MOSI;
static const uint8_t MISO = PIN_SPI_MISO;
static const uint8_t SCK = PIN_SPI_SCK;

// I2C (TWIM30): SDA P0.03, SCL P0.04, external 4.7k pull-ups required
#define WIRE_INTERFACES_COUNT 1
#define PIN_WIRE_SDA 3
#define PIN_WIRE_SCL 4
#define WIRE_TWIM NRF_TWIM30
#define WIRE_TWIS NRF_TWIS30
#define WIRE_IRQN SERIAL30_IRQn
#define WIRE_IRQ_HANDLER SERIAL30_IRQHandler

#ifdef __cplusplus
}
#endif

#undef LORA_SCK
#undef LORA_MISO
#undef LORA_MOSI
#undef LORA_CS

#define LORA_SCK PIN_SPI_SCK
#define LORA_MOSI PIN_SPI_MOSI
#define LORA_MISO PIN_SPI_MISO
#define LORA_CS SS

#define LORA_DIO0 RADIOLIB_NC
#define LORA_RESET 64
#define LORA_DIO1 2 // IRQ - Delivers sub-microsecond hardware interrupts via GPIOTE
#define LORA_DIO2 67 // BUSY
#define LORA_DIO3 RADIOLIB_NC

//#define USE_SX1262
#define USE_LR1121

// SX1262
#ifdef USE_SX1262
#define SX126X_CS SS
#define SX126X_DIO1 LORA_DIO1
#define SX126X_BUSY LORA_DIO2
#define SX126X_RESET LORA_RESET
// RXEN is held high permanently (LNA always on); TXEN follows DIO2.
#define SX126X_RXEN 71
#define SX126X_TXEN RADIOLIB_NC
#define SX126X_DIO2_AS_RF_SWITCH
#define SX126X_DIO3_TCXO_VOLTAGE 1.8
#endif

#ifdef USE_LR1121
#define LR1121_IRQ_PIN LORA_DIO1
#define LR1121_NRESET_PIN LORA_RESET
#define LR1121_BUSY_PIN LORA_DIO2
#define LR1121_SPI_NSS_PIN SS
#define LR1121_SPI_SCK_PIN SCK
#define LR1121_SPI_MOSI_PIN MOSI
#define LR1121_SPI_MISO_PIN MISO
#define LR11X0_DIO3_TCXO_VOLTAGE 1.8
#define LR11X0_DIO_AS_RF_SWITCH
#define IRQ_DIO_NUM 8
#endif
