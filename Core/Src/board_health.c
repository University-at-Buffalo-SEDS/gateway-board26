#include "board_health.h"
#include "board_temperature.h"
#include "board_uptime.h"
#include "board_watchdog.h"
#include "telemetry.h"
#include "main.h"
#include <stdint.h>
#include <stdbool.h>
#if __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "Board diagnostic words require little-endian encoding"
#endif
#ifdef TELEMETRY_USE_TLSF
extern volatile uint32_t g_telemetry_tlsf_live_bytes, g_telemetry_tlsf_peak_bytes, g_telemetry_tlsf_region_bytes, g_telemetry_tlsf_failures;
#endif
extern uint32_t board_health_can_rx_drops(void);
extern int telemetry_router_enter(void);
extern int telemetry_router_leave(void);
extern volatile uint32_t g_can_tx_rejected, g_can_tx_high_water;
extern volatile uint32_t g_gateway_uart_rx_ring_drops;
extern volatile uint32_t g_power_ltc2990_present, g_power_i2c_retry_delay_ms, g_power_i2c_retries;
extern volatile uint32_t g_power_i2c_recoveries, g_power_i2c_recovery_failures;
/* "LTC1" little-endian: words 17..21 are the power-monitor block. */
#define BOARD_HEALTH_POWER_TAG 0x3143544CU

void board_health_poll(SedsRouter *router)
{
#ifdef TELEMETRY_ENABLED
  static uint32_t last_report_ms;
  static uint32_t failures;
  if (!router) return;
  const uint64_t uptime = board_uptime_ticks();
  const uint32_t now = (uint32_t)uptime;
  if ((uint32_t)(now - last_report_ms) < 5000U) return;
  last_report_ms = now;
  board_temperature_poll();

  uint32_t words[24] = {1U, 2U, (uint32_t)uptime, (uint32_t)(uptime >> 32U), BOARD_UPTIME_FREQUENCY_HZ};
  const uint32_t saved = __get_PRIMASK(); __disable_irq();
  words[5] = g_watchdog_reset_flags; words[6] = g_watchdog_missing_mask;
#ifdef TELEMETRY_USE_TLSF
  /* Exact existing occupied-byte counters. Avoid interrupt-disabled heap
   * walks, and do not mislabel occupied headroom as contiguous free space. */
  words[1] |= 1U;
  words[7] = g_telemetry_tlsf_live_bytes; words[8] = g_telemetry_tlsf_peak_bytes;
  words[9] = g_telemetry_tlsf_region_bytes; words[10] = g_telemetry_tlsf_failures;
#endif
  words[12] = board_health_can_rx_drops();
  words[1] |= 4U | 64U; words[13] = g_can_tx_rejected; words[15] = g_can_tx_high_water;
  words[1] |= 8U; words[14] = g_gateway_uart_rx_ring_drops;
  /* Power monitors: w17 bit0 voltage / bit1 current LTC2990 answering
   * (clear = absent, not trusted); w18 current retry delay in ms (0 when
   * healthy); w19 retries; w20 bus clears; w21 failed bus clears. */
  words[16] = BOARD_HEALTH_POWER_TAG; words[17] = g_power_ltc2990_present;
  words[18] = g_power_i2c_retry_delay_ms; words[19] = g_power_i2c_retries;
  words[20] = g_power_i2c_recoveries; words[21] = g_power_i2c_recovery_failures;
  __set_PRIMASK(saved);
  words[11] = (uint32_t)seds_memory_admission_rejected();
  words[23] = failures;
  words[1] |= board_temperature_health_flags();
  if (!telemetry_router_enter()) { ++failures; return; }
  /* Static UInt32 schema + exact little-endian bytes: use the existing
   * bounded byte path without pulling every generic numeric converter in. */
  const SedsResult result = seds_router_log_bytes_ex(router, SEDS_DT_BOARD_HEALTH,
      (const uint8_t *)words, sizeof(words), NULL, true);
  const int released = telemetry_router_leave();
  if (result != SEDS_OK || !released) ++failures;
#else
  (void)router;
#endif
}
