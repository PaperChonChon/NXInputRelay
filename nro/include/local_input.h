#pragma once

#include "switch_gamepad.h"

#include <stdbool.h>
#include <stdint.h>
#include <switch.h>

/* A hid:dbg-backed virtual controller and keyboard consumed by Switch 1.
   The backend is acquired only for an authenticated local network session. */
bool local_input_begin(Result *out_result);
bool local_input_ready(void);
Result local_input_set_gamepad(const SwitchGamepadReport *report);
Result local_input_set_keyboard(uint8_t modifier, uint8_t key);
Result local_input_set_touch(bool down,uint16_t x,uint16_t y);
Result local_input_neutral(void);
void local_input_end(void);

