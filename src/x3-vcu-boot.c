/**
 * \file            x3-vcu-boot.c
 * \brief           Segway-Ninebot x3 VCU bootloader (Artery AT32F415)
 *
 * \copyright       Copyright (c) 2026 hp197. Licensed under CC BY-NC-SA 4.0.
 *                      SPDX-License-Identifier: CC-BY-NC-SA-4.0
 *                      The Artery AT32F415 BSP + CMSIS used at build time keep
 *                      their own licenses (see LICENSE, "Third-party components").
 */

/*
 * Occupies the first `4 KB` of flash. On boot it brings up the clock and flash,
 * reads the boot descriptor, and - if an OTA is pending - commits it (erase
 * `slot 0`, copy `slot 1` -> `slot 0`). It then validates and launches the
 * application in `slot 0`. There is no comms/DFU-receive path: the application
 * itself receives an OTA image and stages it into `slot 1`.
 *
 * Flash layout:
 *   0x08000000  bootloader (this, 4 KB)
 *   0x08001000  slot 0  - active application
 *   0x08010000  slot 1  - OTA staging
 *   0x0801F800  boot descriptor (in the identity sector)
 *
 * Built on the official Artery AT32F415 BSP (BSD-3-Clause) + CMSIS.
 */
#include "at32f415.h"

#define SLOT0         0x08001000u /*!< active application                 */
#define SLOT1         0x08010000u /*!< OTA staging                        */
#define DESC_FLASH    0x0801F800u /*!< boot descriptor in identity sector */
#define SECTOR        0x800u      /*!< 2 KB flash sector                  */
#define SRAM_MASK     0x2FFE0000u /*!< mask that isolates the SRAM region */
#define SRAM_MATCH    0x20000000u /*!< expected SRAM base after masking   */
#define COPY_STEP     0x100u      /*!< OTA copy granularity, in bytes     */
#define DELAY_STEP_MS 50u         /*!< delay chunk, keeps SysTick in 24-bit range */

/**
 * \brief           Boot descriptor stored in the identity sector
 */
typedef struct {
    uint32_t magic;  /*!< record marker                 */
    uint32_t flag;   /*!< non-zero when an OTA is staged */
    uint32_t length; /*!< staged image length, in bytes */
} descriptor_t;

static descriptor_t g_desc;
static uint8_t g_copybuf[COPY_STEP];
static volatile uint8_t g_reboot;
static uint32_t g_ticks_per_ms;

/**
 * \brief           Bring up the system clock (`HEXT` -> `PLL`)
 */
static void clock_config(void) {
    crm_reset();
    flash_psr_set(FLASH_WAIT_CYCLE_4);

    crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
    while (crm_hext_stable_wait() == ERROR) {}

    /* HEXT is 16 MHz on this board: HEXT/2 * 18 = 144 MHz */
    crm_pll_config(CRM_PLL_SOURCE_HEXT_DIV, CRM_PLL_MULT_18);
    crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
    while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) {}

    crm_ahb_div_set(CRM_AHB_DIV_1);
    crm_apb2_div_set(CRM_APB2_DIV_2);
    crm_apb1_div_set(CRM_APB1_DIV_2);
    crm_auto_step_mode_enable(TRUE);

    crm_sysclk_switch(CRM_SCLK_PLL);
    while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) {}
    crm_auto_step_mode_enable(FALSE);

    system_core_clock_update();
}

/**
 * \brief           Erase a flash region, one `2 KB` sector at a time
 * \param[in]       addr: first address of the region to erase
 * \param[in]       len: region length, in bytes
 * \return          `1` when the whole region was erased, `0` on any failure
 */
static uint8_t flash_erase_region(uint32_t addr, uint32_t len) {
    uint32_t addr_cur, addr_end;
    uint8_t res = 1;

    flash_unlock();
    addr_end = addr + len;
    for (addr_cur = addr & ~(SECTOR - 1); addr_cur < addr_end; addr_cur += SECTOR) {
        if (flash_sector_erase(addr_cur) != FLASH_OPERATE_DONE) {
            res = 0;
            break;
        }
    }
    flash_lock();
    return res;
}

/**
 * \brief           Program flash half-word by half-word and read-back verify
 * \param[in]       dst: destination flash address
 * \param[in]       src: source half-word buffer
 * \param[in]       nbytes: number of bytes to program
 * \return          `1` when every half-word was written and verified, else `0`
 */
static uint8_t flash_program_verify(uint32_t dst, const uint16_t* src, uint32_t nbytes) {
    uint32_t done;

    flash_unlock();
    for (done = 0; done < nbytes; done += 2, dst += 2, ++src) {
        if (flash_halfword_program(dst, *src) != FLASH_OPERATE_DONE) {
            break;
        }
        if (*(volatile uint16_t*)dst != *src) {
            break;
        }
    }
    flash_lock();
    return done >= nbytes ? 1 : 0;
}

/**
 * \brief           Copy a block of memory, byte by byte
 * \param[out]      dst: destination buffer
 * \param[in]       src: source buffer
 * \param[in]       len: number of bytes to copy
 */
static void mem_copy(void* dst, const void* src, uint32_t len) {
    uint8_t* dp = dst;
    const uint8_t* sp = src;

    while (len > 0) {
        *dp++ = *sp++;
        --len;
    }
}

/**
 * \brief           Persist the in-RAM boot descriptor back to flash
 * \return          `1` when the descriptor was written and verified, else `0`
 */
static uint8_t commit_descriptor(void) {
    uint8_t res = 0;

    if (flash_erase_region(DESC_FLASH, sizeof(g_desc))) {
        res = flash_program_verify(DESC_FLASH, (const uint16_t*)&g_desc, sizeof(g_desc));
    }
    return res;
}

/**
 * \brief           Read the boot descriptor from flash into RAM
 * \note            Garbage (`magic != 0`) means no OTA pending.
 */
static void load_descriptor(void) {
    mem_copy(&g_desc, (const void*)DESC_FLASH, sizeof(g_desc));
    if (g_desc.magic != 0) {
        g_desc.magic = 0;
        g_desc.flag = 0;
        g_desc.length = 0;
    }
}

/**
 * \brief           Set `MSP` from a slot and branch to its reset vector
 * \param[in]       base: slot base address (vector table)
 * \note            Does not return: control passes to the application
 */
static void slot_gate_launch(uint32_t base) {
    uint32_t sp = *(volatile uint32_t*)base;
    uint32_t rv = *(volatile uint32_t*)(base + 4);

    __set_MSP(sp);
    ((void (*)(void))rv)();
    for (;;) {}
}

/**
 * \brief           Launch a slot when its stored stack pointer is sane
 * \param[in]       base: slot base address (vector table)
 * \return          `0` when the stack pointer is invalid (launch is skipped);
 *                      on success control does not return
 */
static uint8_t slot_gate(uint32_t base) {
    uint8_t res = 0;

    if ((*(volatile uint32_t*)base & SRAM_MASK) == SRAM_MATCH) {
        slot_gate_launch(base);
    }
    return res;
}

/**
 * \brief           Commit a staged OTA: erase `slot 0` and copy `slot 1` into it
 * \return          `0` on success, or a non-zero reason code on failure
 */
static uint8_t validate_slot(void) {
    uint32_t off;
    uint8_t res;

    if (g_desc.length == 0) {
        return 1;
    }

    res = 0;
    if ((*(volatile uint32_t*)SLOT1 & SRAM_MASK) != SRAM_MATCH) {
        res = 9;
    } else if (!flash_erase_region(SLOT0, g_desc.length)) {
        res = 2;
    } else {
        for (off = 0; off < g_desc.length; off += COPY_STEP) {
            mem_copy(g_copybuf, (const void*)(SLOT1 + off), COPY_STEP);
            if (!flash_program_verify(SLOT0 + off, (const uint16_t*)g_copybuf, COPY_STEP)) {
                res = 3;
                break;
            }
        }
    }
    return res;
}

/**
 * \brief           Commit a staged OTA if flagged, then launch `slot 0`
 */
static void boot_decision(void) {
    if (!g_desc.flag) {
        if (!slot_gate(SLOT0)) {
            g_reboot = 1;
        }
    } else {
        __disable_irq();
        if (validate_slot() == 0) {
            g_desc.flag = 0;
            commit_descriptor();
            if (!slot_gate(SLOT0)) {
                g_reboot = 1;
            }
        } else {
            g_reboot = 1;
        }
        __enable_irq();
    }
}

/**
 * \brief           Prepare the `SysTick` millisecond busy-wait
 * \note            Run after `clock_config()`; selects `HCLK` as the `SysTick`
 *                      source and caches ticks/ms.
 */
static void delay_init(void) {
    systick_clock_source_config(SYSTICK_CLOCK_SOURCE_AHBCLK_NODIV);
    g_ticks_per_ms = system_core_clock / 1000u;
}

/**
 * \brief           Busy-wait a number of milliseconds by polling `SysTick`
 * \param[in]       ms: delay duration, in milliseconds
 * \note            Chunked by `DELAY_STEP_MS` to stay within the 24-bit `SysTick`
 *                      reload.
 */
static void delay_ms(uint32_t ms) {
    uint32_t chunk;

    while (ms > 0) {
        chunk = ms > DELAY_STEP_MS ? DELAY_STEP_MS : ms;
        SysTick->LOAD = (chunk * g_ticks_per_ms) - 1u;
        SysTick->VAL = 0;
        SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;
        while ((SysTick->CTRL & SysTick_CTRL_COUNTFLAG_Msk) == 0) {}
        SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;
        ms -= chunk;
    }
}

/**
 * \brief           Bootloader entry; resets to retry if no application launches
 * \return          Never returns
 */
int main(void) {
    clock_config();
    delay_init();
    g_reboot = 0;
    load_descriptor();
    boot_decision();
    while (!g_reboot) {
        delay_ms(1000);
    }
    NVIC_SystemReset();
    for (;;) {}
}

/*
 * startup_at32f415.s calls __libc_init_array; these are the C-runtime init/fini
 * hooks (fixed libc names). They are empty here - there are no constructors to
 * run - and weak so a newlib/framework-provided definition wins if present.
 */

/**
 * \brief           C-runtime global constructors hook (unused)
 */
__attribute__((weak)) void _init(void) {}

/**
 * \brief           C-runtime global destructors hook (unused)
 */
__attribute__((weak)) void _fini(void) {}
