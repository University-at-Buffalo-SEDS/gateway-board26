#ifndef BOARD_TELEMETRY_MUTEX_H
#define BOARD_TELEMETRY_MUTEX_H
#include "tx_api.h"
#include <stdatomic.h>
#include <stddef.h>

/* One ThreadX acquisition retains priority inheritance and exclusion across
 * tasks. SDK state locks recursively enter this same board-owned mutex many
 * times per packet; only the owning task may elide those recursive API calls.
 * ISR/startup exclusion remains the caller's responsibility. */
typedef struct {
    _Atomic(TX_THREAD *) owner;
    unsigned depth;
} BoardTelemetryMutex;

static inline UINT board_telemetry_mutex_get(TX_MUTEX *mutex,
                                             BoardTelemetryMutex *state,
                                             TX_THREAD *self) {
    if (atomic_load_explicit(&state->owner, memory_order_acquire) == self) {
        ++state->depth;
        return TX_SUCCESS;
    }
    UINT status = tx_mutex_get(mutex, TX_WAIT_FOREVER);
    if (status == TX_SUCCESS) {
        state->depth = 1U;
        atomic_store_explicit(&state->owner, self, memory_order_release);
    }
    return status;
}
static inline UINT board_telemetry_mutex_put(TX_MUTEX *mutex,
                                             BoardTelemetryMutex *state,
                                             TX_THREAD *self) {
    if (atomic_load_explicit(&state->owner, memory_order_acquire) != self ||
        state->depth == 0U) return TX_NOT_OWNED;
    if (--state->depth != 0U) return TX_SUCCESS;
    /* A waiting task can run inside tx_mutex_put: publish release first and
     * never overwrite the successor's owner/depth after the kernel handoff. */
    atomic_store_explicit(&state->owner, NULL, memory_order_release);
    return tx_mutex_put(mutex);
}
#endif
