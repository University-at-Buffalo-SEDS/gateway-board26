#pragma once
#include <stddef.h>
#include <stdint.h>
/* Returns 1 with the unfinalized IEEE CRC state, or 0 for software fallback. */
int32_t gateway_crc32_update(uint32_t state, const uint8_t *bytes, size_t len,
                           uint32_t *result);
