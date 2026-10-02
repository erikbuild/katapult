// ABOUTME: Page-level helpers for the stm32 flash driver: locate a page in
// ABOUTME: dual-bank flash and check whether a flash range is erased.
//
// Copyright (C) 2026  Erik Reynolds <erik@hartunions.com>
//
// This file may be distributed under the terms of the GNU GPLv3 license.
#ifndef __STM32_FLASH_PAGES_H
#define __STM32_FLASH_PAGES_H

#include <stdint.h> // uint32_t

// Check if 'count' bytes starting at 'start' are erased (all 0xff)
static inline int
flash_range_is_erased(const void *start, uint32_t count)
{
    const uint32_t *p = start, *e = p + count / 4;
    while (p < e)
        if (*p++ != 0xffffffff)
            return 0;
    return 1;
}

struct flash_bank_page {
    uint32_t bank, page;
};

// Locate the page holding 'offset' (bytes from the start of flash) in
// flash made of two equal banks of 'bank_size' bytes divided into
// 'page_size' byte pages. Bank 0 is the first physical bank. When
// 'banks_swapped' is set the first half of the address space is served
// by the second physical bank.
static inline struct flash_bank_page
flash_bank_page_lookup(uint32_t offset, uint32_t bank_size
                       , uint32_t page_size, int banks_swapped)
{
    uint32_t bank = offset >= bank_size;
    if (banks_swapped)
        bank = !bank;
    return (struct flash_bank_page){
        .bank = bank, .page = (offset % bank_size) / page_size };
}

#endif // flash_pages.h
