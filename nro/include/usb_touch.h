#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <switch.h>

typedef struct {
    bool ready;
    bool descriptor_valid;
    bool transfer_pending;
    uint32_t reports_completed;
    uint32_t reports_failed;
} UsbTouchStatus;

/* Experimental single-contact absolute USB HID touchscreen for Switch 2. */
const UsbTouchStatus *usb_touch_status(void);
bool usb_touch_ready(void);
Result usb_touch_send(bool down,uint16_t x,uint16_t y,uint64_t now_ms);
void usb_touch_cancel_transfer(void);

