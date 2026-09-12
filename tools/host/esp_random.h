#pragma once
#include <stdint.h>
static inline uint32_t esp_random(void) {
    static uint32_t state = 0x12345678;
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}
