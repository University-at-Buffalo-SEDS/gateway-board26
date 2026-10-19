#ifndef TELEMETRY_TLSF_H
#define TELEMETRY_TLSF_H
#include "tx_api.h"
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
extern volatile uint32_t g_gateway_admission_last_free;
extern volatile uint32_t g_gateway_admission_last_additional;
extern volatile uint32_t g_gateway_admission_last_largest;
#ifdef SEDS_FIRMWARE_SIM_TEST
extern volatile uint32_t g_gateway_admission_last_reason;
/* Lower64 occupancy bits; full pool counters include all128 slots. */
uint64_t telemetry_tlsf_reserve_mask(void);
#endif
/* Register startup pools before the first Rust allocation. TLSF then owns
 * their remaining contiguous storage; ThreadX keeps thread-stack ownership. */
void telemetry_tlsf_register_pool(TX_BYTE_POOL *pool);
void *telemetry_tlsf_malloc(size_t size);
void telemetry_tlsf_free(void *ptr);
void telemetry_tlsf_sample(void);
bool telemetry_tlsf_admit(size_t additional, size_t largest);
/* Bounded async priority-CAN copy; retains a full working estimate. */
bool telemetry_tlsf_admit_can_control(size_t bytes);
bool telemetry_tlsf_admit_cached_transit(size_t additional, size_t largest);
bool telemetry_tlsf_admit_small_protected(size_t additional, size_t largest);
#endif
