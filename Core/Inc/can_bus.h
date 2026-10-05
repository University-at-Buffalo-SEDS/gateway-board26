#pragma once

#include <stddef.h>
#include <stdint.h>
#include "stm32g4xx_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*can_bus_rx_cb_t)(const uint8_t *data, size_t len, void *user);

/* Init with the FDCAN handle that receives on FIFO1 (e.g. &hfdcan2). */
void can_bus_init(FDCAN_HandleTypeDef *hfdcan);
/* False after initialization/recovery failure; watchdog must not be fed. */
int can_bus_health_ok(void);

/* Nonblocking raw hardware enqueue (len clamped to 64); HAL_BUSY if full. */
HAL_StatusTypeDef can_bus_send_bytes(const uint8_t *bytes, size_t len, uint32_t std_id);

/* Copy up to 128 bytes into the async fragmentation queue. HAL_OK means
 * accepted, not acknowledged. HAL_BUSY refuses the whole packet unchanged. */
HAL_StatusTypeDef can_bus_send_large(const uint8_t *bytes, size_t len, uint32_t std_id);

/*
 * MUST be called periodically from thread/main-loop context.
 * This drains the ISR RX ring, performs reassembly, and invokes subscribers.
 */
void can_bus_process_rx(void);
/* Bounded service for interleaving command ingress and router acknowledgements. */
uint32_t can_bus_process_rx_budget(uint32_t max_frames);
/* Deadline checked after each frame; one synchronous callback can overrun it. */
uint32_t can_bus_process_rx_for(uint32_t max_frames, uint32_t max_ms);
uint32_t can_bus_rx_pending(void);

/* Number of frames rejected because the ISR-to-thread ring was full. */
uint32_t can_bus_rx_dropped_frames(void);

#ifdef CAN_BUS_TEST
/* Host-test ingress hook; never present in production firmware. */
void can_bus_test_inject(uint32_t std_id, const uint8_t *data, size_t len);
#endif


/*
 * Subscribe a callback to RX events (FIFO1).
 * Can be called at startup before interrupts start firing.
 * Returns HAL_OK on success, HAL_ERROR if the list is full or duplicate.
 */
HAL_StatusTypeDef can_bus_subscribe_rx(can_bus_rx_cb_t cb, void *user);

/*
 * Optional: remove a previously added subscription.
 * Returns HAL_OK if removed, HAL_ERROR if not found.
 */
HAL_StatusTypeDef can_bus_unsubscribe_rx(can_bus_rx_cb_t cb, void *user);

#ifdef __cplusplus
}
#endif
