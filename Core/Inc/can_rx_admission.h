#ifndef CAN_RX_ADMISSION_H
#define CAN_RX_ADMISSION_H
#include <stddef.h>
#include <stdint.h>
#include "sedsnet_config.h"
#include "can_link.h"

/* Admission only: recognize frequent loadcell/pressure samples, never battery
 * values or valve status. Fragmented packets keep normal access (link format
 * v2: only a SINGLE frame holds a whole packet; see can_link.h).
 * No templates, heap allocations, or changes to routing/schema semantics. */
static inline int can_rx_is_bulk_telemetry(uint32_t id, const uint8_t *p, size_t n)
{
  if (!p || !can_link_is_single(id)) return 0;
  n = can_link_payload_len(id, p, n);
  if (n < 3U) return 0;
  /* SEDSnet 7: a FULL template frame (0x60) prefixes its template
   * id/generation and base delta ULEB128s to the canonical data; other
   * side-transport kinds (both 0x60 bits set) are never discarded here. */
  if (p[0] == 0x60U) {
    size_t i = 1U;
    for (unsigned field = 0U; field < 2U; field++) {
      unsigned count = 0U;
      do {
        if (i >= n || count++ == 5U) return 0;
      } while (p[i++] & 0x80U);
    }
    p += i; n -= i;
  } else if ((p[0] & 0x60U) == 0x60U) {
    return 0;
  }
  /* Canonical prelude: flags, then the data type as a u32 ULEB128 (SEDSnet
   * wire_format.rs). Hashed type IDs take up to five bytes, so the whole ID
   * is decoded. Only ordinary best-effort samples qualify: reliable or sealed
   * frames stay protected, and the router still performs full decoding and
   * validation. */
  if (n < 6U || (p[0] & 0x12U) || (p[0] & 0x60U) == 0x60U) return 0;
  uint32_t type = 0U;
  for (unsigned i = 0U;; i++) {
    if (i == 5U || 1U + i >= n) return 0;
    const uint8_t b = p[1U + i];
    if (i == 4U && (b & 0xf0U)) return 0; /* over 32 bits */
    type |= (uint32_t)(b & 0x7fU) << (7U * i);
    if (!(b & 0x80U)) break;
  }
  return type == (uint32_t)SEDS_DT_KG1000 || type == (uint32_t)SEDS_DT_KG50 ||
         type == (uint32_t)SEDS_DT_OX_TANK_PRESSURE;
}
#endif
