#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Explicit CRC32 Data profile. Uses caller-owned existing RX storage; no packet
 * decoding, allocation, schema knowledge or wire-format change. After failed
 * Data integrity or framing noise, Command/ASCII candidates are quarantined
 * until CRC-valid Data restores alignment. Genuine diagnostics in that span
 * are unavailable; clean-boundary commands and the separate opaque profile
 * keep their existing behavior. */
typedef struct {
    uint8_t *bytes;
    size_t capacity, used, cursor, incomplete, checked_through;
    bool tainted, needs_input;
    uint32_t rejected_data, quarantined_commands;
} UartIntegrityReader;
typedef struct { unsigned scans, crc_bytes; } UartIntegrityBudget;
typedef bool (*UartIntegrityCheck)(const uint8_t *, size_t);
typedef void (*UartIntegrityEmit)(uint8_t, const uint8_t *, size_t);
typedef enum { UART_INTEGRITY_NEEDS_INPUT, UART_INTEGRITY_BUDGET } UartIntegrityResult;

void uart_integrity_init(UartIntegrityReader *, uint8_t *, size_t);
size_t uart_integrity_space(const UartIntegrityReader *);
bool uart_integrity_push(UartIntegrityReader *, uint8_t);
UartIntegrityResult uart_integrity_drain(UartIntegrityReader *, UartIntegrityBudget *,
                                       UartIntegrityCheck, UartIntegrityEmit);

/* Stop only at candidate boundaries. A deadline never becomes a failed CRC or
 * resets retained bytes; context is borrowed synchronously, never retained. */
typedef bool (*UartIntegrityContinue)(const void *);
UartIntegrityResult uart_integrity_drain_until(UartIntegrityReader *, UartIntegrityBudget *,
    UartIntegrityCheck, UartIntegrityEmit, UartIntegrityContinue, const void *);
