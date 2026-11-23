#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Resolve either a decimal/0x-prefixed HID usage or a descriptive key name. */
bool network_key_parse(const char *text,uint8_t *usage);

typedef struct {
    uint8_t modifier;
    uint8_t usage;
} NetworkKeyStroke;

bool network_key_parse_chord(const char *text,bool allow_modifier_only,NetworkKeyStroke *stroke);
bool network_modifier_parse(const char *text,uint8_t *modifier);

