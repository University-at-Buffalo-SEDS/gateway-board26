#pragma once
#include <stdint.h>

/* Enqueue activity is not evidence of a frame reaching the bus. */
typedef struct {
    uint32_t since_ms, completions;
    uint8_t waiting;
} can_transport_health;

static inline int can_transport_stalled(can_transport_health *h,
        uint32_t now, uint32_t pending, uint32_t completions) {
    if (!pending || !h->waiting || completions != h->completions) {
        h->since_ms = now;
        h->completions = completions;
        h->waiting = pending != 0U;
        return 0;
    }
    return (uint32_t)(now - h->since_ms) >= 1000U;
}
