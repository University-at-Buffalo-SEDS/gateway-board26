#include "uart_integrity.h"
#include <string.h>

void uart_integrity_init(UartIntegrityReader *reader, uint8_t *bytes, size_t capacity) {
    *reader = (UartIntegrityReader){.bytes=bytes, .capacity=capacity,
                                  .incomplete=SIZE_MAX, .needs_input=true};
}
size_t uart_integrity_space(const UartIntegrityReader *reader) {
    return reader->capacity - reader->used;
}
bool uart_integrity_push(UartIntegrityReader *reader, uint8_t byte) {
    if (!uart_integrity_space(reader)) return false;
    reader->bytes[reader->used++] = byte;
    reader->needs_input = false;
    return true;
}
static void discard(UartIntegrityReader *reader, size_t count) {
    memmove(reader->bytes, reader->bytes + count, reader->used - count);
    reader->used -= count;
    reader->checked_through = reader->checked_through > count
        ? reader->checked_through - count : 0;
}
static void accepted(UartIntegrityReader *reader, size_t at, size_t length,
                     uint8_t kind, UartIntegrityEmit emit) {
    /* The callback borrows these bytes only for this call. Advance ownership
     * only after it returns; never scan a valid parent's payload for framing. */
    emit(kind, reader->bytes + at + 4, length);
    discard(reader, at + 4 + length);
    reader->cursor = 0;
    reader->incomplete = SIZE_MAX;
    reader->checked_through = 0;
    reader->tainted = false;
}
UartIntegrityResult uart_integrity_drain_until(UartIntegrityReader *reader,
    UartIntegrityBudget *budget, UartIntegrityCheck check, UartIntegrityEmit emit,
    UartIntegrityContinue proceed, const void *context) {
    if (reader->needs_input) return UART_INTEGRITY_NEEDS_INPUT;
    for (;;) {
        if (proceed != NULL && !proceed(context)) return UART_INTEGRITY_BUDGET;
        if (!budget->scans || !budget->crc_bytes) return UART_INTEGRITY_BUDGET;
        const size_t at = reader->tainted ? reader->cursor : 0;
        if (reader->used >= at + 4) {
            const uint8_t kind = reader->bytes[at];
            const uint8_t second = reader->bytes[at + 1];
            const bool known = (kind == 0xa5 && second == 0x5a) ||
                               (kind == 0xa6 && second == 0x5b) ||
                               (kind == 0xa7 && second == 0x7a);
            --budget->scans;
            if (!known) {
                if (reader->tainted) ++reader->cursor;
                else {
                    /* Framing noise may be a damaged Data header. Its payload
                     * must not authorize embedded local Command/ASCII frames. */
                    reader->tainted = true;
                    reader->cursor = 1;
                    reader->incomplete = SIZE_MAX;
                }
                continue;
            }
            const size_t length = reader->bytes[at + 2] |
                                  ((size_t)reader->bytes[at + 3] << 8);
            if (length > reader->capacity - 4) {
                if (reader->tainted) ++reader->cursor;
                else { reader->tainted=true; reader->cursor=1; reader->incomplete=SIZE_MAX; }
                continue;
            }
            if (reader->tainted && kind != 0xa5) {
                ++reader->quarantined_commands;
                ++reader->cursor;
                continue;
            }
            const size_t end = at + 4 + length;
            if (reader->used >= end) {
                if (kind != 0xa5) { accepted(reader, at, length, kind, emit); continue; }
                if (end > reader->checked_through) {
                    if (length > budget->crc_bytes) return UART_INTEGRITY_BUDGET;
                    budget->crc_bytes -= (unsigned)length;
                    if (check(reader->bytes + at + 4, length)) {
                        accepted(reader, at, length, kind, emit);
                        continue;
                    }
                    ++reader->rejected_data;
                }
                if (reader->tainted) ++reader->cursor;
                else { reader->tainted=true; reader->cursor=1; reader->incomplete=SIZE_MAX; }
                continue;
            }
            if (reader->tainted) {
                if (at < reader->incomplete) reader->incomplete = at;
                ++reader->cursor;
                continue;
            }
        } else if (reader->tainted) {
            reader->checked_through = reader->used;
            const size_t keep = reader->incomplete != SIZE_MAX ? reader->incomplete
                : (reader->used > 3 ? reader->used - 3 : 0);
            discard(reader, keep);
            reader->cursor = 0;
            reader->incomplete = SIZE_MAX;
        }
        reader->needs_input = true;
        return UART_INTEGRITY_NEEDS_INPUT;
    }
}

UartIntegrityResult uart_integrity_drain(UartIntegrityReader *reader,
    UartIntegrityBudget *budget, UartIntegrityCheck check, UartIntegrityEmit emit) {
    return uart_integrity_drain_until(reader, budget, check, emit, NULL, NULL);
}
