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
  /* Known one-byte IDs, ordinary best-effort samples only. Keep frames with
   * reliable/unknown flags protected. Existing native loadcell types are
   * non-reliable; the router still performs full decoding and validation. */
  if (n < 7U || (p[0] & 0xd2U)) return 0;
  return p[2] == SEDS_DT_KG1000 || p[2] == SEDS_DT_KG50 ||
         p[2] == SEDS_DT_FUEL_TANK_PRESSURE;
}
#endif
