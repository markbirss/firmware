/*
 * nrf54l15_main.cpp — Zephyr entry point for Meshtastic nRF54L15 port
 *
 * Zephyr calls main() instead of Arduino's setup()/loop().
 * This file provides the main() that bootstraps the Arduino-style
 * Meshtastic application loop.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>

// Forward declarations from src/main.cpp
void setup();
void loop();

// ── Crash info saved to noinit RAM (survives soft reset) ─────────────────────
struct crash_info {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc;
    uint32_t sp;
    uint32_t lr;
    uint32_t cfsr;   // Configurable Fault Status Register
};
static struct crash_info saved_crash __attribute__((section(".noinit")));
#define CRASH_MAGIC 0xDEADBEEF

// Override Zephyr's weak fatal handler to save crash info before reboot
extern "C" void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    saved_crash.magic  = CRASH_MAGIC;
    saved_crash.reason = reason;
    if (esf) {
        saved_crash.pc   = esf->basic.pc;
        saved_crash.sp   = esf->basic.xpsr;  // use xpsr as placeholder (sp not in esf)
        saved_crash.lr   = esf->basic.lr;
    }
    // Read Cortex-M33 SCB CFSR
    saved_crash.cfsr = *((volatile uint32_t *)0xE000ED28U);
    printk("[nrf54l15] FATAL reason=%u pc=0x%08x lr=0x%08x cfsr=0x%08x\n",
           reason, saved_crash.pc, saved_crash.lr, saved_crash.cfsr);
    k_fatal_halt(reason);
}

int main(void)
{
    uint32_t reset_cause = 0;
    hwinfo_get_reset_cause(&reset_cause);
    hwinfo_clear_reset_cause();
    printk("[nrf54l15] Reset cause: 0x%08x\n", reset_cause);

    if (saved_crash.magic == CRASH_MAGIC) {
        printk("[nrf54l15] Prev crash: reason=%u pc=0x%08x lr=0x%08x cfsr=0x%08x\n",
               saved_crash.reason, saved_crash.pc, saved_crash.lr, saved_crash.cfsr);
        saved_crash.magic = 0;
    }

    printk("[nrf54l15] A: main() entry\n");
    printk("[nrf54l15] B: calling setup()\n");
    setup();
    printk("[nrf54l15] C: setup() returned\n");
    while (true) {
        loop();
    }
    return 0;
}
