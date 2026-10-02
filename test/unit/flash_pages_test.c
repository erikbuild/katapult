// ABOUTME: Host unit tests for the stm32 flash page helpers in src/stm32/flash_pages.h.
// ABOUTME: Built and run on the build machine by scripts/test-unit.sh.

#include <stdio.h> // printf
#include <string.h> // memset
#include "stm32/flash_pages.h"

static int failures;

#define CHECK(cond) do {                                            \
        if (!(cond)) {                                              \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);  \
            failures++;                                             \
        }                                                           \
    } while (0)

static void
check_page(uint32_t offset, uint32_t bank_size, int swapped
           , uint32_t bank, uint32_t page)
{
    struct flash_bank_page bp = flash_bank_page_lookup(
        offset, bank_size, 8 * 1024, swapped);
    if (bp.bank != bank || bp.page != page) {
        printf("FAIL offset 0x%x bank_size 0x%x swapped %d:"
               " got bank %u page %u, want bank %u page %u\n"
               , offset, bank_size, swapped, bp.bank, bp.page, bank, page);
        failures++;
    }
}

static void
test_range_is_erased(void)
{
    uint32_t buf[16];
    memset(buf, 0xff, sizeof(buf));
    CHECK(flash_range_is_erased(buf, sizeof(buf)));
    buf[15] = 0; // last word programmed
    CHECK(!flash_range_is_erased(buf, sizeof(buf)));
    buf[15] = 0xffffffff;
    buf[4] = 0x12345678;
    CHECK(!flash_range_is_erased(buf, sizeof(buf)));
    CHECK(flash_range_is_erased(buf, 16)); // first four words only
}

static void
test_bank_page_lookup(void)
{
    // 512KiB part: two 256KiB banks of 32 pages
    check_page(0x00000, 0x40000, 0, 0, 0);
    check_page(0x02000, 0x40000, 0, 0, 1);
    check_page(0x3e000, 0x40000, 0, 0, 31);
    check_page(0x40000, 0x40000, 0, 1, 0);
    check_page(0x7e000, 0x40000, 0, 1, 31);
    // 256KiB part: two 128KiB banks of 16 pages
    check_page(0x1e000, 0x20000, 0, 0, 15);
    check_page(0x20000, 0x20000, 0, 1, 0);
    check_page(0x3e000, 0x20000, 0, 1, 15);
    // A bank swap exchanges the physical banks, not the page numbers
    check_page(0x02000, 0x40000, 1, 1, 1);
    check_page(0x42000, 0x40000, 1, 0, 1);
}

int
main(void)
{
    test_range_is_erased();
    test_bank_page_lookup();
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("flash_pages: all tests passed\n");
    return 0;
}
