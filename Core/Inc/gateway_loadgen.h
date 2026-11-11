#ifndef GATEWAY_LOADGEN_H
#define GATEWAY_LOADGEN_H
/* Synthetic CAN ingress for throughput measurement. Present only when the
 * build sets GATEWAY_LOADGEN_FPS (CMake -DGATEWAY_LOADGEN_FPS=<frames/s>);
 * never enable it in flight or ground-test firmware.
 *
 * From the 1 kHz HAL tick ISR, the generator pushes canonical, CRC-valid
 * SEDSnet KG1000 packets (sender "LG", GroundStation endpoint) into the CAN
 * receive ring through the same rb_push() the FDCAN ISR uses. Every packet
 * carries a unique counter so SEDSnet duplicate suppression does not drop
 * it. The FDCAN peripheral and HAL_FDCAN_GetRxMessage are bypassed; their
 * per-frame cost is measured separately in the GCP_CAN_ISR profile slot. */
#include <stdint.h>

#ifdef GATEWAY_LOADGEN_FPS
/* Frames per second; writable from a debugger. 0 pauses injection. */
extern volatile uint32_t g_gateway_loadgen_fps;
extern volatile uint32_t g_gateway_loadgen_injected;
/* Milliseconds after the template is built before injection starts. */
extern volatile uint32_t g_gateway_loadgen_start_ms;
/* Thread context, once per telemetry loop: builds the template once. */
void gateway_loadgen_poll(void);
void gateway_loadgen_tick(void);
#else
#define gateway_loadgen_poll() ((void)0)
#define gateway_loadgen_tick() ((void)0)
#endif

#endif
