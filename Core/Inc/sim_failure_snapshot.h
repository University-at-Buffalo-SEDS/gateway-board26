#ifndef SIM_FAILURE_SNAPSHOT_H
#define SIM_FAILURE_SNAPSHOT_H

#ifdef SEDS_FIRMWARE_SIM_TEST
#include <stdint.h>

/* Simulator-only diagnostic words, never built into flight images. Layout:
 *   [0, 18)   router memory counters (telemetry.c)
 *   [20, 36)  router queue counters (telemetry.c)
 *   [62]      PSP at the first dropped CAN frame, [63] its 'CAND' tag
 *   [64, 96)  task stack words at the first dropped CAN frame (can_bus.c)
 * An allocation failure overwrites all words with the task stack at the
 * failure. 96 words keep the instrumented image inside the 112 KiB SRAM. */
#define SIM_FAILURE_SNAPSHOT_WORDS 96U
#define SIM_FAILURE_SNAPSHOT_CAN_FRAME_FIRST 64U
#define SIM_FAILURE_SNAPSHOT_RAM_END 0x2001C000U

extern volatile uint32_t g_telemetry_alloc_failure_stack[SIM_FAILURE_SNAPSHOT_WORDS];
#endif

#endif
