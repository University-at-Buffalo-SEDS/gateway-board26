#ifndef CAN_RX_ADMISSION_H
#define CAN_RX_ADMISSION_H
#include <stddef.h>
#include <stdint.h>
#include "sedsnet_config.h"

/* Admission only: recognize frequent loadcell/pressure samples, never battery
 * values or valve status. Unknown/fragmented packets keep normal access.
 * No templates, heap allocations, or changes to routing/schema semantics. */
static inline int can_rx_is_bulk_telemetry(const uint8_t *p, size_t n)
{
  if (!p || n < 3U) return 0;
  if (p[0] == 'S' && p[1] == 'D' && p[2] != 'T') {
    if (n < 9U || p[4] != 0U || p[5] != 1U) return 0;
    const size_t total = p[7] | ((size_t)p[8] << 8);
    if (total > n - 9U) return 0;
    p += 9U; n = total;
  }
  if (n >= 3U && p[0] == 'S' && p[1] == 'D' && p[2] == 'T') {
    if (n < 5U || p[3] != 1U) return 0;
    size_t i = 4U;
    /* Full-template IDs are u32 ULEB; compact frames are never discarded here. */
    unsigned count = 0U;
    do {
      if (i >= n || count++ == 5U) return 0;
    } while (p[i++] & 0x80U);
    p += i; n -= i;
  }
  /* Frame prelude: flags, endpoint count, then the data type as a u32
   * ULEB128 (SEDSnet wire_format.rs). Hashed type IDs take up to five bytes,
   * so the whole ID is decoded; comparing only p[2] matched nothing. Only
   * ordinary best-effort samples qualify: frames with reliable/unknown flags
   * stay protected, and the router still performs full decoding/validation. */
  if (n < 7U || (p[0] & 0xd2U)) return 0;
  uint32_t type = 0U;
  for (unsigned i = 0U;; i++) {
    if (i == 5U || 2U + i >= n) return 0;
    const uint8_t b = p[2U + i];
    if (i == 4U && (b & 0xf0U)) return 0; /* over 32 bits */
    type |= (uint32_t)(b & 0x7fU) << (7U * i);
    if (!(b & 0x80U)) break;
  }
  return type == (uint32_t)SEDS_DT_KG1000 || type == (uint32_t)SEDS_DT_KG50 ||
         type == (uint32_t)SEDS_DT_OX_TANK_PRESSURE;
}
#endif
