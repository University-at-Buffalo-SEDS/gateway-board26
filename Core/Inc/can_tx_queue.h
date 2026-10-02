#pragma once
#include "can_bus.h"
/* Foreground submit copies the packet. HAL_OK means queued, not bus ACKed. */
HAL_StatusTypeDef can_tx_queue_submit(const uint8_t *data, size_t len, uint32_t id);
/* Thread service reclaims memory; IRQ pump never allocates/frees. */
void can_tx_queue_service(void);
void can_tx_queue_pump(void);
void can_tx_queue_reset(void);
