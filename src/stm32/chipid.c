// Support for extracting the hardware chip id on stm32
//
// Copyright (C) 2019  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include "generic/canserial.h" // canserial_set_uuid
#include "generic/usb_cdc.h" // usb_fill_serial
#include "generic/usbstd.h" // usb_string_descriptor
#include "internal.h" // UID_BASE
#include "sched.h" // DECL_INIT

#define CHIP_UID_LEN 12

static struct {
    struct usb_string_descriptor desc;
    uint16_t data[CHIP_UID_LEN * 2];
} cdc_chipid;

struct usb_string_descriptor *
usbserial_get_serialid(void)
{
   return &cdc_chipid.desc;
}

void
chipid_init(void)
{
    // Copy the id with 32-bit reads; some chips (stm32c5) fault on
    // byte reads of the area holding the unique id
    uint32_t uid[CHIP_UID_LEN / 4];
    const volatile uint32_t *uid_reg = (void*)UID_BASE;
    int i;
    for (i = 0; i < ARRAY_SIZE(uid); i++)
        uid[i] = uid_reg[i];

    if (CONFIG_USB_SERIAL_NUMBER_CHIPID)
        usb_fill_serial(&cdc_chipid.desc, ARRAY_SIZE(cdc_chipid.data), uid);
    if (CONFIG_CANBUS)
        canserial_set_uuid((uint8_t *)uid, CHIP_UID_LEN);
}
DECL_INIT(chipid_init);
