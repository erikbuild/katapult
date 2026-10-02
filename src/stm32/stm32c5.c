// ABOUTME: Clock, flash wait state and peripheral clock setup for the stm32c5 family.
// ABOUTME: Katapult copy of Kalico's stm32c5.c without the instruction cache and ADC clock.
//
// Copyright (C) 2026  Erik Reynolds <me@erik.build>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "autoconf.h" // CONFIG_CLOCK_REF_FREQ
#include "board/armcm_boot.h" // armcm_main
#include "board/armcm_reset.h" // try_request_canboot
#include "board/irq.h" // irq_disable
#include "board/misc.h" // bootloader_request
#include "command.h" // DECL_CONSTANT_STR
#include "internal.h" // enable_pclock
#include "sched.h" // sched_main


/****************************************************************
 * Clock setup
 ****************************************************************/

// All bus prescalers stay at /1, so peripherals run at the system clock
#define FREQ_PERIPH CONFIG_CLOCK_FREQ

// Map a peripheral address to its enable bits
struct cline
lookup_clock_line(uint32_t periph_base)
{
    if (periph_base < APB2PERIPH_BASE) {
        uint32_t pos = (periph_base - APB1PERIPH_BASE) / 0x400;
        if (pos < 32)
            return (struct cline){.en = &RCC->APB1LENR,
                                  .rst = &RCC->APB1LRSTR,
                                  .bit = 1 << pos};
        return (struct cline){.en = &RCC->APB1HENR,
                              .rst = &RCC->APB1HRSTR,
                              .bit = 1 << (pos - 32)};
    } else if (periph_base < AHB1PERIPH_BASE) {
        uint32_t pos = (periph_base - APB2PERIPH_BASE) / 0x400;
        return (struct cline){.en = &RCC->APB2ENR,
                              .rst = &RCC->APB2RSTR,
                              .bit = 1 << pos};
    } else {
        // Above APB2 the drivers only enable AHB2 peripherals (gpio, adc)
        if (periph_base == ADC12_COMMON_BASE)
            return (struct cline){.en = &RCC->AHB2ENR,
                                  .rst = &RCC->AHB2RSTR,
                                  .bit = RCC_AHB2ENR_ADC12EN};
        uint32_t pos = (periph_base - AHB2PERIPH_BASE) / 0x400;
        return (struct cline){.en = &RCC->AHB2ENR,
                              .rst = &RCC->AHB2RSTR,
                              .bit = 1 << pos};
    }
}

// Return the frequency of the given peripheral clock
uint32_t
get_pclock_frequency(uint32_t periph_base)
{
    return FREQ_PERIPH;
}

// Enable a GPIO peripheral clock
void
gpio_clock_enable(GPIO_TypeDef *regs)
{
    uint32_t rcc_pos = ((uint32_t)regs - GPIOA_BASE) / 0x400;
    RCC->AHB2ENR |= 1 << rcc_pos;
    RCC->AHB2ENR;
}

#if !CONFIG_STM32_CLOCK_REF_INTERNAL
DECL_CONSTANT_STR("RESERVE_PINS_crystal", "PH0,PH1");
#endif

// RCC field values (RM0522 section 9.8)
#define SW_HSIDIV3 0
#define SW_HSIS 1
#define SW_PSIS 3
#define PSIFREQ_144MHZ 1
#define PSIREFSRC_HSE 0
#define CK48SEL_PSIDIV3 1
#define CK48SEL_HSIDIV3 2

// PSI reference frequency code for the external crystal
#if CONFIG_CLOCK_REF_FREQ == 8000000
  #define PSIREF_CRYSTAL 1
#elif CONFIG_CLOCK_REF_FREQ == 16000000
  #define PSIREF_CRYSTAL 2
#elif CONFIG_CLOCK_REF_FREQ == 24000000
  #define PSIREF_CRYSTAL 3
#endif

// Switch the system clock source and wait for the switch to complete
static void
switch_sysclk(uint32_t sw)
{
    RCC->CFGR1 = sw << RCC_CFGR1_SW_Pos;
    while ((RCC->CFGR1 & RCC_CFGR1_SWS) != (sw << RCC_CFGR1_SWS_Pos))
        ;
}

// Return clocks to their reset state in case a bootloader changed them
static void
rcc_reset(void)
{
    RCC->CR1 |= RCC_CR1_HSIDIV3ON;
    while (!(RCC->CR1 & RCC_CR1_HSIDIV3RDY))
        ;
    switch_sysclk(SW_HSIDIV3);
    RCC->CFGR2 = 0;
    RCC->CR1 = RCC_CR1_HSIDIV3ON;
    uint32_t psi_rdy = RCC_CR1_PSISRDY | RCC_CR1_PSIDIV3RDY | RCC_CR1_PSIKRDY;
    while (RCC->CR1 & psi_rdy)
        ;
    RCC->CR2 = 0;
    RCC->CCIPR1 = 0;
    RCC->CCIPR2 = 0;
    RCC->AHB2ENR = 0;
    RCC->APB1LENR = 0;
    RCC->APB1HENR = 0;
    RCC->APB2ENR = 0;
    RCC->APB3ENR = 0;
}

// The flash "empty" flag survives a system reset. Once the boot address
// holds code, clear it so the next reset does not start the ROM bootloader.
static void
clear_flash_empty_flag(void)
{
    uint32_t boot_word = *(volatile uint32_t *)CONFIG_FLASH_BOOT_ADDRESS;
    if ((FLASH->ACR & FLASH_ACR_EMPTY) && boot_word != 0xffffffff)
        FLASH->ACR &= ~FLASH_ACR_EMPTY;
}

// Set flash wait states for 144MHz (RM0522 Table 20). These are valid at
// any HCLK up to 144MHz, so they are set before any clock is changed.
static void
flash_setup(void)
{
    uint32_t acr = FLASH->ACR & ~(FLASH_ACR_LATENCY | FLASH_ACR_WRHIGHFREQ);
    acr |= ((4 << FLASH_ACR_LATENCY_Pos) | (2 << FLASH_ACR_WRHIGHFREQ_Pos)
            | FLASH_ACR_PRFTEN);
    FLASH->ACR = acr;
    while ((FLASH->ACR & FLASH_ACR_LATENCY) != (4 << FLASH_ACR_LATENCY_Pos))
        ;
}

// Main clock setup called at chip startup
static void
clock_setup(void)
{
    uint32_t ck48sel;
#if CONFIG_STM32_CLOCK_REF_INTERNAL
    // Run from the internal 144MHz oscillator (HSIS)
    RCC->CR1 |= RCC_CR1_HSISON;
    while (!(RCC->CR1 & RCC_CR1_HSISRDY))
        ;
    switch_sysclk(SW_HSIS);
    // USB uses HSI/3, trimmed from the host's start-of-frame by the CRS
    ck48sel = CK48SEL_HSIDIV3;
    if (CONFIG_USB) {
        enable_pclock(CRS_BASE);
        CRS->CR |= CRS_CR_AUTOTRIMEN | CRS_CR_CEN;
    }
#else
    // Run from the PSI locked to the external crystal (HSE)
    RCC->CR1 |= RCC_CR1_HSEON;
    while (!(RCC->CR1 & RCC_CR1_HSERDY))
        ;
    RCC->CR2 = ((PSIFREQ_144MHZ << RCC_CR2_PSIFREQ_Pos)
                | (PSIREF_CRYSTAL << RCC_CR2_PSIREF_Pos)
                | (PSIREFSRC_HSE << RCC_CR2_PSIREFSRC_Pos));
    uint32_t psi_rdy = RCC_CR1_PSISRDY | RCC_CR1_PSIDIV3RDY;
    RCC->CR1 |= RCC_CR1_PSISON | RCC_CR1_PSIDIV3ON;
    while ((RCC->CR1 & psi_rdy) != psi_rdy)
        ;
    switch_sysclk(SW_PSIS);
    // USB uses PSI/3 (48MHz)
    ck48sel = CK48SEL_PSIDIV3;
#endif

    // USB kernel clock
    RCC->CCIPR2 = ck48sel << RCC_CCIPR2_CK48SEL_Pos;
}


/****************************************************************
 * Bootloader
 ****************************************************************/

// Handle USB reboot requests
void
bootloader_request(void)
{
    try_request_canboot();
    dfu_reboot();
}


/****************************************************************
 * Startup
 ****************************************************************/

// Main entry point - called from armcm_boot.c:ResetHandler()
void
armcm_main(void)
{
    SCB->VTOR = (uint32_t)VectorTable;

    // Flash wait states first, so no clock change below outruns the flash
    flash_setup();

    // Reset clock registers (in case a bootloader changed them)
    rcc_reset();

    dfu_reboot_check();

    clear_flash_empty_flag();
    clock_setup();

    sched_main();
}
