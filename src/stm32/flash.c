// Flash (IAP) functionality for STM32
//
// Copyright (C) 2021 Eric Callahan <arksine.code@gmail.com
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memset
#include "autoconf.h" // CONFIG_MACH_STM32F103
#include "board/io.h" // writew
#include "flash.h" // flash_write_block
#include "flash_pages.h" // flash_range_is_erased
#include "internal.h" // FLASH

// Return the flash page size at the given address
static uint32_t
flash_get_page_size(uint32_t addr)
{
    if (CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4) {
        if (addr < 0x08010000)
            return 16 * 1024;
        else if (addr < 0x08020000)
            return 64 * 1024;
        else
            return 128 * 1024;
    } else if (CONFIG_MACH_STM32F103) {
        // Check for a 1K page size on the stm32f103
        uint16_t *flash_size = (void*)FLASHSIZE_BASE;
        return *flash_size < 256 ? 1024 : 2 * 1024;
    } else if (CONFIG_MACH_STM32F0) {
        if (CONFIG_MACH_STM32F042)
            return 1024;
        if (CONFIG_MACH_STM32F072)
            return 2 * 1024;
        uint16_t *flash_size = (void*)FLASHSIZE_BASE;
        return *flash_size <= 64 ? 1024 : 2 * 1024;
    } else if (CONFIG_MACH_STM32G0 || CONFIG_MACH_STM32G4) {
        return 2 * 1024;
    } else if (CONFIG_MACH_STM32H7) {
        return 128 * 1024;
    } else if (CONFIG_MACH_STM32C5) {
        return 8 * 1024;
    }
}

// Some chips have slightly different register names
#if CONFIG_MACH_STM32G0
#define FLASH_SR_BSY (FLASH_SR_BSY1 | FLASH_SR_BSY2)
#elif CONFIG_MACH_STM32H7
#define CR CR1
#define SR SR1
#define KEYR KEYR1
#endif

// Wait for flash hardware to report ready
static void
wait_flash(void)
{
    while (FLASH->SR & FLASH_SR_BSY)
        ;
}

#ifndef FLASH_KEY1 // Some stm32 headers don't define this
#define FLASH_KEY1 (0x45670123UL)
#define FLASH_KEY2 (0xCDEF89ABUL)
#endif

// Issue low-level flash hardware unlock sequence
static void
unlock_flash(void)
{
    if (FLASH->CR & FLASH_CR_LOCK) {
        // Unlock Flash Erase
        FLASH->KEYR = FLASH_KEY1;
        FLASH->KEYR = FLASH_KEY2;
    }
    wait_flash();
}

// Place low-level flash hardware into a locked state
static void
lock_flash(void)
{
    FLASH->CR = FLASH_CR_LOCK;
}

// Clear error flags left by an earlier operation; on stm32c5 any flag
// still set turns the next erase or program into an error
static void
clear_flash_errors(void)
{
#if CONFIG_MACH_STM32C5
    FLASH->CCR = (FLASH_CCR_CLR_EOP | FLASH_CCR_CLR_WRPERR
                  | FLASH_CCR_CLR_PGSERR | FLASH_CCR_CLR_STRBERR
                  | FLASH_CCR_CLR_INCERR | FLASH_CCR_CLR_OPTCHANGEERR);
#endif
}

// Issue a low-level flash hardware erase request for a flash page
static void
erase_page(uint32_t page_address)
{
#if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4
    uint32_t sidx;
    if (page_address < 0x08010000)
        sidx = (page_address - 0x08000000) / (16 * 1024);
    else if (page_address < 0x08020000)
        sidx = 4;
    else
        sidx = 5 + (page_address - 0x08020000) / (128 * 1024);
    sidx = sidx > 0x0f ? 0x0f : sidx;
    FLASH->CR = (FLASH_CR_PSIZE_1 | FLASH_CR_STRT | FLASH_CR_SER
                 | (sidx << FLASH_CR_SNB_Pos));
#elif CONFIG_MACH_STM32F0 || CONFIG_MACH_STM32F1
    FLASH->CR = FLASH_CR_PER;
    FLASH->AR = page_address;
    FLASH->CR = FLASH_CR_PER | FLASH_CR_STRT;
#elif CONFIG_MACH_STM32G0 || CONFIG_MACH_STM32G4
    uint32_t pidx = (page_address - 0x08000000) / (2 * 1024);
    if (pidx >= 64) {
        uint16_t *flash_size = (void*)FLASHSIZE_BASE;
        if (*flash_size <= 256)
            pidx = pidx + 256 - 64;
        else
            pidx = pidx < 128 ? pidx : pidx + 256 - 128;
    }
    pidx = pidx > 0x3ff ? 0x3ff : pidx;
    FLASH->CR = FLASH_CR_PER | FLASH_CR_STRT | (pidx << FLASH_CR_PNB_Pos);
#elif CONFIG_MACH_STM32H7
    uint32_t snb = (page_address - 0x08000000) / (128 * 1024);
    snb = snb > 7 ? 7 : snb;
    FLASH->CR = FLASH_CR_SER | FLASH_CR_START | (snb << FLASH_CR_SNB_Pos);
    while (FLASH->SR & FLASH_SR_QW)
        ;
    SCB_InvalidateDCache_by_Addr((void*)page_address, 128*1024);
#elif CONFIG_MACH_STM32C5
    // Pages are numbered within each of the two equal physical banks
    uint16_t *flash_size = (void*)FLASHSIZE_BASE;
    uint32_t bank_size = *flash_size * 1024 / 2;
    // OPTCR holds the bank swap in effect; OPTSR_CUR may hold a pending one
    int swapped = !!(FLASH->OPTCR & FLASH_OPTCR_SWAP_BANK);
    struct flash_bank_page bp = flash_bank_page_lookup(
        page_address - CONFIG_FLASH_START, bank_size, 8 * 1024, swapped);
    uint32_t cr = (FLASH_CR_PER | (bp.page << FLASH_CR_PNB_Pos)
                   | (bp.bank ? FLASH_CR_BKSEL : 0));
    FLASH->CR = cr;
    FLASH->CR = cr | FLASH_CR_STRT;
#endif
    wait_flash();
}

// Write out a "block" of data to the low-level flash hardware
static void
write_block(uint32_t block_address, uint32_t *data)
{
#if CONFIG_MACH_STM32F2 || CONFIG_MACH_STM32F4
    uint32_t *page = (void*)block_address;
    FLASH->CR = FLASH_CR_PSIZE_1 | FLASH_CR_PG;
    for (int i = 0; i < CONFIG_BLOCK_SIZE / 4; i++) {
        writel(&page[i], data[i]);
        wait_flash();
    }
#elif CONFIG_MACH_STM32F0 || CONFIG_MACH_STM32F1
    uint16_t *page = (void*)block_address, *data16 = (void*)data;
    FLASH->CR = FLASH_CR_PG;
    for (int i = 0; i < CONFIG_BLOCK_SIZE / 2; i++) {
        writew(&page[i], data16[i]);
        wait_flash();
    }
#elif CONFIG_MACH_STM32G0 || CONFIG_MACH_STM32G4
    uint32_t *page = (void*)block_address;
    FLASH->CR = FLASH_CR_PG;
    for (int i = 0; i < CONFIG_BLOCK_SIZE / 8; i++) {
        writel(&page[i*2], data[i*2]);
        writel(&page[i*2 + 1], data[i*2 + 1]);
        wait_flash();
    }
#elif CONFIG_MACH_STM32H7
    uint32_t *page = (void*)block_address;
    FLASH->CR = FLASH_CR_PG;
    for (int i = 0; i < CONFIG_BLOCK_SIZE / 32; i++) {
        writel(&page[i*8], data[i*8]);
        writel(&page[i*8 + 1], data[i*8 + 1]);
        writel(&page[i*8 + 2], data[i*8 + 2]);
        writel(&page[i*8 + 3], data[i*8 + 3]);
        writel(&page[i*8 + 4], data[i*8 + 4]);
        writel(&page[i*8 + 5], data[i*8 + 5]);
        writel(&page[i*8 + 6], data[i*8 + 6]);
        writel(&page[i*8 + 7], data[i*8 + 7]);
        while (FLASH->SR & FLASH_SR_QW)
            ;
        wait_flash();
    }
    SCB_InvalidateDCache_by_Addr((void*)block_address, CONFIG_BLOCK_SIZE);
#elif CONFIG_MACH_STM32C5
    // Program 128-bit flash words, four 32-bit writes each
    uint32_t *page = (void*)block_address;
    FLASH->CR = FLASH_CR_PG;
    for (int i = 0; i < CONFIG_BLOCK_SIZE / 16; i++) {
        writel(&page[i*4], data[i*4]);
        writel(&page[i*4 + 1], data[i*4 + 1]);
        writel(&page[i*4 + 2], data[i*4 + 2]);
        writel(&page[i*4 + 3], data[i*4 + 3]);
        while (FLASH->SR & (FLASH_SR_BSY | FLASH_SR_WBNE | FLASH_SR_DBNE))
            ;
    }
#endif
}

static uint32_t page_write_count;

// Main block write interface
int
flash_write_block(uint32_t block_address, uint32_t *data)
{
    if (block_address & (CONFIG_BLOCK_SIZE - 1))
        // Not a block aligned address
        return -1;
#if CONFIG_MACH_STM32C5
    // The SINGLE_BANK option places all user flash in one bank, and RM0522
    // does not describe the erase page numbering for that layout, so refuse
    // to write rather than risk erasing the wrong page
    if (FLASH->OPTSR_CUR & FLASH_OPTSR_CUR_SINGLE_BANK)
        return -4;
#endif
    uint32_t flash_page_size = flash_get_page_size(block_address);
    uint32_t page_address = ALIGN_DOWN(block_address, flash_page_size);

    // Check if erase is needed
    int need_erase = 0;
    if (page_address == block_address) {
        if (flash_range_is_erased((void*)block_address, flash_page_size)) {
            // Page already erased
        } else if (memcmp(data, (void*)block_address, CONFIG_BLOCK_SIZE) == 0
                   && flash_range_is_erased((void*)(block_address
                                                    + CONFIG_BLOCK_SIZE)
                                            , flash_page_size
                                              - CONFIG_BLOCK_SIZE)) {
            // Retransmitted request - just ignore
            return 0;
        } else {
            need_erase = 1;
        }
        page_write_count++;
    } else {
        if (!flash_range_is_erased((void*)block_address, CONFIG_BLOCK_SIZE)) {
            if (memcmp(data, (void*)block_address, CONFIG_BLOCK_SIZE) == 0)
                // Retransmitted request - just ignore
                return 0;
            // Block not erased - out of order request?
            return -2;
        }
    }

    // make sure flash is unlocked
    clear_flash_errors();
    unlock_flash();

    // Erase page
    if (need_erase) {
        erase_page(page_address);

        // avoid triggering STM32H72xx write security
        if (CONFIG_MACH_STM32H7) {
            lock_flash();
            unlock_flash();
        }
    }
    // Write block
    write_block(block_address, data);

    lock_flash();

    if (memcmp(data, (void*)block_address, CONFIG_BLOCK_SIZE) != 0)
        // Failed to write to flash?!
        return -3;

    return 0;
}

// Main flash complete notification interface
int
flash_complete(void)
{
    return page_write_count;
}
