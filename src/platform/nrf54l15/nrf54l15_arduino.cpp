/**
 * nrf54l15_arduino.cpp — Arduino shim implementations for Zephyr/nRF54L15
 *
 * Provides concrete implementations for Print, HardwareSerial, GPIO, SPI,
 * and String methods declared in Arduino.h / SPI.h.
 *
 * Phase 3: real GPIO via Zephyr GPIO API and real SPI via Zephyr SPI API.
 * Pin numbering convention: P0.n = n, P1.n = 16+n, P2.n = 32+n.
 */

#include "Arduino.h"
#include "SPI.h"
#include "Wire.h"
#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/spi.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>

// ── Bluefruit singleton stub (satisfies NodeDB.cpp ARCH_NRF52 path) ──────────
#include "bluefruit.h"
BlueFruitClass Bluefruit;

// ── Filesystem singleton (stub for Phase 2) ──────────────────────────────────
#include "InternalFileSystem.h"
Adafruit_LittleFS_Namespace::InternalFileSystem InternalFS;

// ── SAADC lock stub (Phase 2) ────────────────────────────────────────────────
#include "Nrf52SaadcLock.h"
namespace concurrency { Lock *nrf52SaadcLock = nullptr; }

// ── SPI / Wire singletons ─────────────────────────────────────────────────────
SPIClass SPI;
SPIClass SPI1;
TwoWire  Wire;
TwoWire  Wire1;

// ── HardwareSerial singletons ────────────────────────────────────────────────
HardwareSerial Serial;
HardwareSerial Serial1;
HardwareSerial Serial2;

// ── Timing functions — C linkage to match extern "C" declarations ────────────
extern "C" uint32_t millis(void)            { return (uint32_t)k_uptime_get_32(); }
extern "C" uint32_t micros(void)            { return (uint32_t)(k_uptime_get() * 1000ULL); }
extern "C" void     delay(uint32_t ms)      { k_sleep(K_MSEC(ms)); }
extern "C" void     delayMicroseconds(uint32_t us) { k_sleep(K_USEC(us)); }
extern "C" void     yield(void)             { k_yield(); }

// ── NVIC_SystemReset — wraps __NVIC_SystemReset from CMSIS core_cm33.h ───────
// core_cm33.h has #define NVIC_SystemReset __NVIC_SystemReset, so undef it
// before defining our own implementation to prevent macro expansion collision.
#pragma push_macro("NVIC_SystemReset")
#undef NVIC_SystemReset
extern "C" void NVIC_SystemReset(void)      { sys_reboot(SYS_REBOOT_COLD); }
#pragma pop_macro("NVIC_SystemReset")

// ── HardwareSerial::write ─────────────────────────────────────────────────────
size_t HardwareSerial::write(uint8_t c)
{
    // TODO(nrf54l15 Phase 3): route through Zephyr UART / USB-CDC console
    // For now use printk so we at least get something over RTT/UART0
    printk("%c", (char)c);
    return 1;
}

size_t HardwareSerial::write(const uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++)
        printk("%c", (char)buf[i]);
    return n;
}

// ── Print::printf ─────────────────────────────────────────────────────────────
int Print::printf(const char *fmt, ...)
{
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n > 0) write((const uint8_t*)buf, (size_t)(n < (int)sizeof(buf) ? n : (int)sizeof(buf) - 1));
    return n;
}

// ── strlcpy — BSD extension not in Zephyr newlib ────────────────────────────
extern "C" size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t len = strlen(src);
    if (size > 0) {
        size_t copy = len < size - 1 ? len : size - 1;
        memcpy(dst, src, copy);
        dst[copy] = '\0';
    }
    return len;
}

// ── Print numeric helpers ─────────────────────────────────────────────────────
static size_t printNumber(Print &p, unsigned long n, uint8_t base)
{
    if (base == 0)
        return p.write((uint8_t)n);

    char buf[8 * sizeof(long) + 1];
    char *end = buf + sizeof(buf) - 1;
    *end = '\0';
    if (n == 0) {
        *--end = '0';
    } else {
        while (n > 0) {
            unsigned long remainder = n % base;
            *--end = (char)(remainder < 10 ? '0' + remainder : 'A' + remainder - 10);
            n /= base;
        }
    }
    return p.write((const uint8_t*)end, strlen(end));
}

static size_t printFloat(Print &p, double number, uint8_t digits)
{
    if (isnan(number))  return p.print("nan");
    if (isinf(number))  return p.print("inf");
    if (number >  4294967040.0 || number < -4294967040.0)
        return p.print("ovf");

    size_t n = 0;
    if (number < 0.0) { n += p.write('-'); number = -number; }

    // Round
    double rounding = 0.5;
    for (uint8_t i = 0; i < digits; i++) rounding /= 10.0;
    number += rounding;

    unsigned long int_part = (unsigned long)number;
    double remainder = number - (double)int_part;
    n += printNumber(p, int_part, 10);
    if (digits > 0) {
        n += p.write('.');
        for (uint8_t i = 0; i < digits; i++) {
            remainder *= 10.0;
            unsigned int d = (unsigned int)remainder;
            n += p.write('0' + d);
            remainder -= d;
        }
    }
    return n;
}

size_t Print::print(unsigned char n, int base) { return printNumber(*this, n, base); }
size_t Print::print(int n, int base)
{
    if (base == 10 && n < 0) {
        size_t r = write('-');
        return r + printNumber(*this, (unsigned long)(-n), base);
    }
    return printNumber(*this, (unsigned long)n, base);
}
size_t Print::print(long n, int base)
{
    if (base == 10 && n < 0) {
        size_t r = write('-');
        return r + printNumber(*this, (unsigned long)(-n), base);
    }
    return printNumber(*this, (unsigned long)n, base);
}
size_t Print::print(unsigned int n, int base)  { return printNumber(*this, n, base); }
size_t Print::print(unsigned long n, int base) { return printNumber(*this, n, base); }
size_t Print::print(float n, int d)            { return printFloat(*this, n, d); }
size_t Print::print(double n, int d)           { return printFloat(*this, n, d); }

// ── String::replace(String, String) ─────────────────────────────────────────
void String::replace(const String &from, const String &to)
{
    if (from.isEmpty() || !_buf) return;
    // Simple O(n²) replace — fine for typical Meshtastic string lengths
    String result;
    const char *p = _buf;
    while (*p) {
        if (strncmp(p, from.c_str(), from.length()) == 0) {
            result += to;
            p += from.length();
        } else {
            result += *p++;
        }
    }
    *this = result;
}

// ═════════════════════════════════════════════════════════════════════════════
// GPIO — Real Zephyr implementation (Phase 3)
// Pin mapping: P0.n = n (0-15), P1.n = 16+n (16-31), P2.n = 32+n (32-47)
// ═════════════════════════════════════════════════════════════════════════════

static const struct device *_gpio_dev_for_pin(uint32_t pin, gpio_pin_t *zpin)
{
    if (pin < 16) {
        *zpin = (gpio_pin_t)pin;
        return DEVICE_DT_GET(DT_NODELABEL(gpio0));
    } else if (pin < 32) {
        *zpin = (gpio_pin_t)(pin - 16);
        return DEVICE_DT_GET(DT_NODELABEL(gpio1));
    } else {
        *zpin = (gpio_pin_t)(pin - 32);
        return DEVICE_DT_GET(DT_NODELABEL(gpio2));
    }
}

void pinMode(uint32_t pin, uint32_t mode)
{
    gpio_pin_t zpin;
    const struct device *dev = _gpio_dev_for_pin(pin, &zpin);
    if (!device_is_ready(dev)) return;

    gpio_flags_t flags;
    switch (mode) {
        case OUTPUT:       flags = GPIO_OUTPUT_INACTIVE; break;
        case INPUT_PULLUP: flags = GPIO_INPUT | GPIO_PULL_UP; break;
        case INPUT_PULLDOWN: flags = GPIO_INPUT | GPIO_PULL_DOWN; break;
        default:           flags = GPIO_INPUT; break;
    }
    gpio_pin_configure(dev, zpin, flags);
}

void digitalWrite(uint32_t pin, uint32_t value)
{
    gpio_pin_t zpin;
    const struct device *dev = _gpio_dev_for_pin(pin, &zpin);
    if (!device_is_ready(dev)) return;
    gpio_pin_set(dev, zpin, (int)value);
}

int digitalRead(uint32_t pin)
{
    gpio_pin_t zpin;
    const struct device *dev = _gpio_dev_for_pin(pin, &zpin);
    if (!device_is_ready(dev)) return 0;
    return gpio_pin_get(dev, zpin);
}

// ─── attachInterrupt — supports up to NRF54L15_MAX_IRQS pins ────────────────
#define NRF54L15_MAX_IRQS 8

struct _PinIrq {
    struct gpio_callback cb;
    voidFuncPtr          user_cb;
    const struct device *dev;
    gpio_pin_t           zpin;
    bool                 used;
};

static _PinIrq _irq_table[NRF54L15_MAX_IRQS];

static void _gpio_irq_dispatch(const struct device *dev, struct gpio_callback *cb, uint32_t pins)
{
    _PinIrq *irq = CONTAINER_OF(cb, _PinIrq, cb);
    if (irq->user_cb) irq->user_cb();
}

void attachInterrupt(uint32_t pin, voidFuncPtr cb, int mode)
{
    gpio_pin_t zpin;
    const struct device *dev = _gpio_dev_for_pin(pin, &zpin);
    if (!device_is_ready(dev)) return;

    // Find a free slot (or reuse existing registration for same pin)
    _PinIrq *slot = nullptr;
    for (int i = 0; i < NRF54L15_MAX_IRQS; i++) {
        if (_irq_table[i].used && _irq_table[i].dev == dev && _irq_table[i].zpin == zpin) {
            // Re-register: remove old callback first
            gpio_remove_callback(dev, &_irq_table[i].cb);
            slot = &_irq_table[i];
            break;
        }
        if (!slot && !_irq_table[i].used) slot = &_irq_table[i];
    }
    if (!slot) return;  // table full

    gpio_flags_t irq_flags;
    switch (mode) {
        case RISING:  irq_flags = GPIO_INT_EDGE_RISING;  break;
        case FALLING: irq_flags = GPIO_INT_EDGE_FALLING; break;
        default:      irq_flags = GPIO_INT_EDGE_BOTH;    break;
    }

    slot->user_cb = cb;
    slot->dev     = dev;
    slot->zpin    = zpin;
    slot->used    = true;

    gpio_pin_configure(dev, zpin, GPIO_INPUT);
    gpio_init_callback(&slot->cb, _gpio_irq_dispatch, BIT(zpin));
    gpio_add_callback(dev, &slot->cb);
    gpio_pin_interrupt_configure(dev, zpin, irq_flags);
}

void detachInterrupt(uint32_t pin)
{
    gpio_pin_t zpin;
    const struct device *dev = _gpio_dev_for_pin(pin, &zpin);
    for (int i = 0; i < NRF54L15_MAX_IRQS; i++) {
        if (_irq_table[i].used && _irq_table[i].dev == dev && _irq_table[i].zpin == zpin) {
            gpio_pin_interrupt_configure(dev, zpin, GPIO_INT_DISABLE);
            gpio_remove_callback(dev, &_irq_table[i].cb);
            _irq_table[i].used = false;
            break;
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// SPI — Real Zephyr implementation using SPIM20 (Phase 3)
// CS is handled by RadioLib via digitalWrite() — hardware CS not used.
// Mode 0 (CPOL=0, CPHA=0), MSB first, 8 MHz (set in DTS overlay).
// ═════════════════════════════════════════════════════════════════════════════

static const struct device *_spi20_dev = DEVICE_DT_GET(DT_NODELABEL(spi20));

// SPI config: Mode 0, MSB first, no hardware CS (RadioLib does it manually)
static const struct spi_config _spi20_cfg = {
    .frequency = 8000000U,
    .operation = SPI_OP_MODE_MASTER | SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
    .slave     = 0,
    .cs        = {}, // CS = NULL → RadioLib handles CS via GPIO
};

uint8_t SPIClass::transfer(uint8_t data)
{
    uint8_t rx = 0;
    struct spi_buf tx_buf = { .buf = &data, .len = 1 };
    struct spi_buf rx_buf = { .buf = &rx,   .len = 1 };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
    spi_transceive(_spi20_dev, &_spi20_cfg, &tx_set, &rx_set);
    return rx;
}

uint16_t SPIClass::transfer16(uint16_t data)
{
    uint8_t tx[2] = { (uint8_t)(data >> 8), (uint8_t)(data & 0xFF) };
    uint8_t rx[2] = { 0, 0 };
    struct spi_buf tx_buf = { .buf = tx, .len = 2 };
    struct spi_buf rx_buf = { .buf = rx, .len = 2 };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };
    spi_transceive(_spi20_dev, &_spi20_cfg, &tx_set, &rx_set);
    return ((uint16_t)rx[0] << 8) | rx[1];
}

void SPIClass::transferBytes(const uint8_t *tx, uint8_t *rx, uint32_t count)
{
    if (!count) return;
    // Zephyr requires non-const buf pointer; cast is safe for tx-only direction
    struct spi_buf tx_buf = { .buf = const_cast<uint8_t *>(tx), .len = count };
    struct spi_buf rx_buf = { .buf = rx, .len = count };
    struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
    struct spi_buf_set rx_set = { .buffers = rx_buf.buf ? &rx_buf : nullptr,
                                  .count   = rx_buf.buf ? 1U : 0U };
    spi_transceive(_spi20_dev, &_spi20_cfg, &tx_set, rx ? &rx_set : nullptr);
}

void SPIClass::transfer(void *buf, size_t count)
{
    if (!count || !buf) return;
    transferBytes(reinterpret_cast<const uint8_t *>(buf),
                  reinterpret_cast<uint8_t *>(buf), (uint32_t)count);
}
