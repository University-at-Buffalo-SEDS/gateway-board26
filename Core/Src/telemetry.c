// telemetry.c
#include "telemetry.h"
#include "flight_state_cache.h"
#include "sim_network_probe.h"
#include "ota_stream.h"

#include "app_threadx.h"
#ifdef TELEMETRY_BOARD_LINK_UART
#include "board_link_uart.h"
#endif
#include "can_bus.h"
#include "main.h"
#include "sedsnet_config.h"
#include "stm32g4xx_hal.h"
#include "telemetry_uart.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef TELEMETRY_ENABLED
static void print_data_no_telem(void *data, size_t len) {
  (void)data;
  (void)len;
}
#endif

#if defined(__GNUC__) || defined(__clang__)
#define UNUSED_FUNCTION __attribute__((unused))
#else
#define UNUSED_FUNCTION
#endif

#ifndef TELEMETRY_TIMESYNC_MASTER_PRIO
#define TELEMETRY_TIMESYNC_MASTER_PRIO (-1)
#endif

#ifndef TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS
#define TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS 5000U
#endif

#ifndef TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS
#define TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS 2000U
#endif

#ifndef TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS
#define TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS 2000U
#endif

#ifndef TX_TIMER_TICKS_PER_SECOND
#error "TX_TIMER_TICKS_PER_SECOND must be defined by ThreadX."
#endif

#define TELEMETRY_TIMESYNC_ROLE_CONSUMER 0U
#define TELEMETRY_TIMESYNC_ROLE_SOURCE 1U
static uint8_t g_can_rx_subscribed = 0U;
#ifdef TELEMETRY_BOARD_LINK_UART
static uint8_t g_board_link_rx_subscribed = 0U;
#endif
static int32_t g_can_side_id = -1;
#ifdef TELEMETRY_BOARD_LINK_UART
static int32_t g_board_link_side_id = -1;
#endif
static uint8_t g_local_unix_valid = 0U;
static uint64_t g_local_unix_ms = 0ULL;

RouterState g_router = {.r = NULL, .created = 0U, .start_time = 0ULL};

SedsResult tx_send(const uint8_t *bytes, size_t len, void *user);

/* Exported simulator/HIL health signals. A linked-bay test requires both a
 * remote discovery topology change and a valid SEDSNet network clock. */
volatile uint32_t g_telemetry_discovery_seen = 0U;
volatile uint32_t g_telemetry_timesync_valid = 0U;
volatile uint32_t g_telemetry_network_ready = 0U;
volatile uint32_t g_telemetry_peer_mask = 0U;
volatile uint32_t g_sim_heartbeat_attempts = 0U;
volatile uint32_t g_sim_heartbeat_ok = 0U;
volatile uint32_t g_sim_heartbeat_fail = 0U;
volatile uint32_t g_sim_heartbeat_wire_tx = 0U;
volatile uint32_t g_sim_uart_egress_peer_mask = 0U;
volatile uint32_t g_sim_can_heartbeat_ingress_mask = 0U;
volatile uint32_t g_sim_can_rx_callback_count = 0U;
volatile uint32_t g_sim_can_heartbeat_count = 0U;
volatile uint32_t g_sim_can_heartbeat_unrecognized = 0U;
volatile uint32_t g_sim_can_last_data_type = 0U;
volatile uint32_t g_sim_can_last_source_address = 0U;
volatile uint32_t g_sim_can_umbilical_status_count = 0U;
volatile uint32_t g_sim_uart_last_data_type = 0U;
volatile uint32_t g_sim_uart_valve_command_count = 0U;
volatile uint32_t g_sim_can_valve_command_tx_count = 0U;
volatile uint32_t g_sim_uart_router_receive_fail = 0U;
volatile int32_t g_sim_uart_router_last_result = 0;
volatile uint32_t g_sim_gateway_pilot_open_status = 0U;

static void telemetry_signal_deserialize_failure(void) {
  /* Keep GREEN_LED reserved for UART activity indication during bring-up. */
}

static uint64_t tx_raw_now_ms_locked(void) {
  const uint32_t ticks32 = (uint32_t)tx_time_get();
  return ((uint64_t)ticks32 * 1000ULL) / (uint64_t)TX_TIMER_TICKS_PER_SECOND;
}

static UNUSED_FUNCTION uint64_t tx_raw_now_ms(void *user) {
  (void)user;
  return tx_raw_now_ms_locked();
}

void telemetry_uart_handle_command(const uint8_t *payload, size_t len) {
  char text[TELEMETRY_UART_MAX_PAYLOAD + 1U];
  size_t text_len = 0U;

  if (!payload || len == 0U) {
    return;
  }

  text_len = (len > TELEMETRY_UART_MAX_PAYLOAD) ? TELEMETRY_UART_MAX_PAYLOAD : len;
  memcpy(text, payload, text_len);
  text[text_len] = '\0';
  printf("[uart cmd] %s", text);
}

void telemetry_uart_handle_raw_ascii(const uint8_t *payload, size_t len) {
  if (!payload || len == 0U) {
    return;
  }

  printf("[uart raw ascii] %.*s\r\n", (int)len, (const char *)payload);
}

void telemetry_uart_handle_data(const uint8_t *payload, size_t len) {
#ifndef TELEMETRY_ENABLED
  (void)payload;
  (void)len;
#else
  SedsResult result = SEDS_OK;

  if (payload == NULL || len == 0U) {
    return;
  }
#ifdef SEDS_FIRMWARE_SIM_TEST
  g_sim_uart_last_data_type = sim_probe_packed_data_type(payload, len);
  if (g_sim_uart_last_data_type == (uint32_t)SEDS_DT_VALVE_COMMAND) {
    g_sim_uart_valve_command_count++;
  }
#endif
  sim_probe_observe_packed(payload, len);

  result = seds_pkt_validate_packed(payload, len);
  if (result != SEDS_OK) {
#ifdef SEDS_FIRMWARE_SIM_TEST
    g_sim_uart_router_receive_fail++;
    g_sim_uart_router_last_result = (int32_t)result;
#endif
    telemetry_uart_note_deserialize_result(0U);
    (void)log_error_asynchronous("UART dropped invalid serialized payload: %d len=%u\r\n",
                                 (int)result, (unsigned)len);
    telemetry_signal_deserialize_failure();
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (telemetry_uart_side_id() >= 0) {
    result = seds_router_receive_packed_from_side(
        g_router.r, (uint32_t)telemetry_uart_side_id(), payload, len);
  } else {
    result = seds_router_receive_packed(g_router.r, payload, len);
  }

  if (result != SEDS_OK) {
    telemetry_uart_note_deserialize_result(0U);
    (void)log_error_asynchronous("UART enqueue failed: %d len=%u\r\n", (int)result,
                                 (unsigned)len);
    (void)print_telemetry_error(result);
    telemetry_signal_deserialize_failure();
    return;
  }

#ifdef SEDS_FIRMWARE_SIM_TEST
  g_sim_uart_router_last_result = (int32_t)result;
#endif
  telemetry_uart_note_deserialize_result(1U);
#endif
}

static uint8_t telemetry_timesync_is_source(void) {
  return (TELEMETRY_TIMESYNC_MASTER_PRIO >= 0) ? 1U : 0U;
}

static uint64_t telemetry_timesync_priority(void) {
  return telemetry_timesync_is_source() ? (uint64_t)TELEMETRY_TIMESYNC_MASTER_PRIO : 0ULL;
}

static uint32_t telemetry_timesync_role(void) {
  return telemetry_timesync_is_source() ? TELEMETRY_TIMESYNC_ROLE_SOURCE
                                        : TELEMETRY_TIMESYNC_ROLE_CONSUMER;
}

static bool telemetry_unix_ms_to_utc(uint64_t unix_ms, int32_t *year, uint8_t *month,
                                     uint8_t *day, uint8_t *hour, uint8_t *minute,
                                     uint8_t *second, uint16_t *millisecond) {
  static const uint16_t days_before_month[12] = {0U,   31U,  59U,  90U,  120U, 151U,
                                                 181U, 212U, 243U, 273U, 304U, 334U};
  uint64_t whole_seconds = unix_ms / 1000ULL;
  const uint64_t days_since_epoch = whole_seconds / 86400ULL;
  uint32_t seconds_of_day = (uint32_t)(whole_seconds % 86400ULL);
  int32_t y = 1970;
  uint64_t days = days_since_epoch;

  if (!year || !month || !day || !hour || !minute || !second || !millisecond) {
    return false;
  }

  while (1) {
    const uint32_t y_u32 = (uint32_t)y;
    const uint8_t leap =
        ((y_u32 % 4U) == 0U && ((y_u32 % 100U) != 0U || (y_u32 % 400U) == 0U)) ? 1U : 0U;
    const uint32_t days_in_year = leap ? 366U : 365U;
    if (days < days_in_year) {
      uint8_t m = 1U;
      uint32_t day_of_year = (uint32_t)days;
      for (; m <= 12U; ++m) {
        uint32_t month_start = days_before_month[m - 1U];
        uint32_t month_end =
            (m < 12U) ? days_before_month[m] : (uint32_t)(leap ? 366U : 365U);
        if (leap && m > 2U) {
          month_start += 1U;
          month_end += 1U;
        }
        if (day_of_year < month_end) {
          *year = y;
          *month = m;
          *day = (uint8_t)(day_of_year - month_start + 1U);
          *hour = (uint8_t)(seconds_of_day / 3600U);
          *minute = (uint8_t)((seconds_of_day % 3600U) / 60U);
          *second = (uint8_t)(seconds_of_day % 60U);
          *millisecond = (uint16_t)(unix_ms % 1000ULL);
          return true;
        }
      }
      return false;
    }
    days -= days_in_year;
    ++y;
  }
}

static SedsResult telemetry_apply_local_unix_time_locked(SedsRouter *router) {
  int32_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
  uint16_t millisecond = 0;

  if (!router || !telemetry_timesync_is_source() || !g_local_unix_valid) {
    return SEDS_OK;
  }

  if (!telemetry_unix_ms_to_utc(g_local_unix_ms, &year, &month, &day, &hour, &minute,
                                &second, &millisecond)) {
    return SEDS_BAD_ARG;
  }

  return seds_router_set_local_network_datetime_millis(router, year, month, day, hour, minute,
                                                       second, millisecond);
}

static SedsResult telemetry_configure_timesync_locked(SedsRouter *router) {
  SedsResult result;

  if (!router) {
    return SEDS_BAD_ARG;
  }

  result = seds_router_configure_timesync(
      router, true, telemetry_timesync_role(), telemetry_timesync_priority(),
      (uint64_t)TELEMETRY_TIMESYNC_SOURCE_TIMEOUT_MS,
      (uint64_t)TELEMETRY_TIMESYNC_ANNOUNCE_INTERVAL_MS,
      (uint64_t)TELEMETRY_TIMESYNC_REQUEST_INTERVAL_MS);
  if (result != SEDS_OK) {
    return result;
  }

  return telemetry_apply_local_unix_time_locked(router);
}

uint64_t telemetry_now_ms(void) { return tx_raw_now_ms_locked(); }

uint64_t telemetry_unix_ms(void) {
#ifndef TELEMETRY_ENABLED
  return g_local_unix_valid ? g_local_unix_ms : 0ULL;
#else
  uint64_t unix_ms = 0ULL;

  if (g_router.r && seds_router_get_network_time_ms(g_router.r, &unix_ms) == SEDS_OK) {
    return unix_ms;
  }

  if (telemetry_timesync_is_source() && g_local_unix_valid) {
    return g_local_unix_ms;
  }

  return 0ULL;
#endif
}

uint64_t telemetry_unix_s(void) { return telemetry_unix_ms() / 1000ULL; }

uint8_t telemetry_unix_is_valid(void) { return telemetry_unix_ms() != 0ULL ? 1U : 0U; }

void telemetry_set_unix_time_ms(uint64_t unix_ms) {
  g_local_unix_ms = unix_ms;
  g_local_unix_valid = (unix_ms != 0ULL) ? 1U : 0U;

#ifdef TELEMETRY_ENABLED
  if (g_router.r != NULL) {
    (void)telemetry_apply_local_unix_time_locked(g_router.r);
  }
#endif
}

static uint64_t node_now_since_ms(void *user) {
  (void)user;
  const RouterState s = g_router;
  const uint64_t now = tx_raw_now_ms_locked();
  return s.r ? (now - s.start_time) : 0ULL;
}

SedsResult tx_send(const uint8_t *bytes, size_t len, void *user) {
  HAL_StatusTypeDef status = HAL_ERROR;
  (void)user;

  if (!bytes || len == 0U) {
    return SEDS_BAD_ARG;
  }
  HAL_GPIO_TogglePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin);

#ifdef SEDS_FIRMWARE_SIM_TEST
  if (sim_probe_packed_data_type(bytes, len) == (uint32_t)SEDS_DT_VALVE_COMMAND) {
    g_sim_can_valve_command_tx_count++;
  }
#endif

  const uint32_t can_id =
      sim_probe_packed_data_type(bytes, len) == (uint32_t)SEDS_DT_HEARTBEAT
          ? 0x004U
          : 0x104U;
  status = can_bus_send_large(bytes, len, can_id);
  if (status == HAL_OK) {
    sim_probe_observe_can_tx(bytes, len);
    return SEDS_OK;
  }

  return SEDS_IO;
}

#ifdef TELEMETRY_BOARD_LINK_UART
static SedsResult board_link_tx_send(const uint8_t *bytes, size_t len, void *user) {
  return board_link_uart_tx_send(bytes, len, user);
}
#endif

static void telemetry_can_rx(const uint8_t *data, size_t len, void *user) {
  (void)user;
  sim_probe_observe_packed(data, len);
#ifdef SEDS_FIRMWARE_SIM_TEST
  g_sim_can_rx_callback_count++;
  g_sim_can_last_data_type = sim_probe_packed_data_type(data, len);
  g_sim_can_last_source_address = sim_probe_packed_source_address(data, len);
  if (g_sim_can_last_data_type == (uint32_t)SEDS_DT_UMBILICAL_STATUS) {
    g_sim_can_umbilical_status_count++;
    g_sim_gateway_pilot_open_status++;
  }
  if (g_sim_can_last_data_type == (uint32_t)SEDS_DT_HEARTBEAT) {
    const uint32_t bit = sim_probe_peer_bit_packed(data, len);
    g_sim_can_heartbeat_count++;
    g_sim_can_heartbeat_ingress_mask |= bit;
    if (bit == 0U) {
      g_sim_can_heartbeat_unrecognized++;
    }
  }
#endif
  /* Tag CAN as the ingress side. Relay mode forwards the original packed wire
   * image to UART and excludes the source side, preventing bridge echoes. */
  rx_asynchronous(data, len);
}

#ifdef TELEMETRY_BOARD_LINK_UART
static void telemetry_board_link_rx(const uint8_t *data, size_t len, void *user) {
  (void)user;

#ifdef TELEMETRY_ENABLED
  SedsResult result = SEDS_OK;

  if (!data || len == 0U) {
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (g_board_link_side_id >= 0) {
    result = seds_router_receive_packed_from_side(
        g_router.r, (uint32_t)g_board_link_side_id, data, len);
  } else {
    result = seds_router_receive_packed(g_router.r, data, len);
  }

  if (result != SEDS_OK) {
    telemetry_signal_deserialize_failure();
  } else {
    g_telemetry_discovery_seen = 1U;
  }
#else
  (void)data;
  (void)len;
#endif
}
#endif

void rx_asynchronous(const uint8_t *bytes, size_t len) {
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  SedsResult result = SEDS_OK;

  if (!bytes || len == 0U) {
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (g_can_side_id >= 0) {
    result = seds_router_receive_packed_from_side(
        g_router.r, (uint32_t)g_can_side_id, bytes, len);
  } else {
    result = seds_router_receive_packed(g_router.r, bytes, len);
  }

  if (result != SEDS_OK) {
    telemetry_signal_deserialize_failure();
  } else {
    g_telemetry_discovery_seen = 1U;
  }
#endif
}

static UNUSED_FUNCTION void rx_synchronous(const uint8_t *bytes, size_t len) {
#ifndef TELEMETRY_ENABLED
  (void)bytes;
  (void)len;
  return;
#else
  if (!bytes || len == 0U) {
    return;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return;
  }

  if (g_can_side_id >= 0) {
    (void)seds_router_receive_packed_from_side(g_router.r, (uint32_t)g_can_side_id, bytes,
                                                   len);
  } else {
    (void)seds_router_receive_packed(g_router.r, bytes, len);
  }
#endif
}

static void telemetry_update_network_health(SedsRouter *router) {
  uint64_t network_time_ms = 0ULL;
  if (seds_router_get_network_time_ms(router, &network_time_ms) == SEDS_OK) {
    g_telemetry_timesync_valid = 1U;
  }
  if (g_telemetry_discovery_seen != 0U &&
      g_telemetry_timesync_valid != 0U) {
    g_telemetry_network_ready = 1U;
  }
}

SedsResult telemetry_poll_timesync(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  const SedsResult result = seds_router_poll_timesync(g_router.r, NULL);
  telemetry_update_network_health(g_router.r);
  return result;
#endif
}

SedsResult telemetry_announce_discovery(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_announce_discovery(g_router.r);
#endif
}

SedsResult telemetry_poll_discovery(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  bool did_queue = false;
  (void)flight_state_cache_poll(g_router.r);
  const SedsResult result = seds_router_poll_discovery(g_router.r, &did_queue);
  if (result == SEDS_OK) {
    sim_probe_emit_heartbeat(g_router.r, telemetry_now_ms());
  }
  telemetry_update_network_health(g_router.r);
  return result;
#endif
}
SedsResult init_telemetry_router(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  SedsRouter *r = NULL;
  SedsResult result = SEDS_OK;
  int32_t uart_side_id = -1;
#ifdef TELEMETRY_BOARD_LINK_UART
  int32_t board_link_side_id = -1;
#endif

  if (g_router.created && g_router.r) {
    return SEDS_OK;
  }

  if (!g_can_rx_subscribed) {
    if (can_bus_subscribe_rx(telemetry_can_rx, NULL) == HAL_OK) {
      g_can_rx_subscribed = 1U;
    } else {
      printf("Error: can_bus_subscribe_rx failed\r\n");
    }
  }

#ifdef TELEMETRY_BOARD_LINK_UART
  if (!g_board_link_rx_subscribed) {
    if (board_link_uart_subscribe_rx(telemetry_board_link_rx, NULL) == HAL_OK) {
      g_board_link_rx_subscribed = 1U;
    } else {
      printf("Error: board_link_uart_subscribe_rx failed\r\n");
    }
  }
#endif

  /* Gateway is a true two-sided SEDSNet bridge. Each physical receiver passes
   * its side id to the router, which preserves the packed frame and never
   * forwards it back to its ingress side. Endpoint reachability is learned by
   * discovery; the gateway must not advertise remote endpoints as local. */
  r = seds_router_new(Seds_RM_Relay, node_now_since_ms, NULL, NULL, 0U);
  if (!r) {
    printf("Error: failed to create router\r\n");
    g_router.r = NULL;
    g_router.created = 0U;
    g_can_side_id = -1;
#ifdef TELEMETRY_BOARD_LINK_UART
    g_board_link_side_id = -1;
#endif
    telemetry_uart_set_side_id(-1);
    return SEDS_ERR;
  }

  g_can_side_id = seds_router_add_side_packed(r, "can", 3U, tx_send, NULL, false);
  if (g_can_side_id < 0) {
    printf("Error: failed to add CAN side: %ld\r\n", (long)g_can_side_id);
    g_can_side_id = -1;
  }

  /* The Pico-Fi/GroundStation transport already owns link delivery. Keep the
   * packed SEDSNet side symmetric with RF's radio side; enabling hop ACKs on
   * only this endpoint leaves every frame awaiting an ACK that RF never emits
   * and exhausts the Gateway pool. */
  uart_side_id = seds_router_add_side_packed(r, "uart", 4U, telemetry_uart_tx_send, NULL, false);
  telemetry_uart_set_side_id(uart_side_id);
  if (uart_side_id < 0) {
    printf("Error: failed to add UART side: %ld\r\n", (long)uart_side_id);
    telemetry_uart_set_side_id(-1);
  }

#ifdef TELEMETRY_BOARD_LINK_UART
  board_link_side_id =
      seds_router_add_side_packed(r, "board-link-uart", 5U, board_link_tx_send, NULL, true);
  g_board_link_side_id = board_link_side_id;
  if (board_link_side_id < 0) {
    printf("Error: failed to add board-link UART side: %ld\r\n", (long)board_link_side_id);
    g_board_link_side_id = -1;
  }
#endif

#ifdef TELEMETRY_BOARD_LINK_UART
  if (g_can_side_id < 0 || uart_side_id < 0 || board_link_side_id < 0) {
#else
  if (g_can_side_id < 0 || uart_side_id < 0) {
#endif
    printf("Error: relay requires configured CAN/UART sides\r\n");
    seds_router_free(r);
    g_router.r = NULL;
    g_router.created = 0U;
    g_can_side_id = -1;
#ifdef TELEMETRY_BOARD_LINK_UART
    g_board_link_side_id = -1;
#endif
    telemetry_uart_set_side_id(-1);
    return SEDS_ERR;
  }

  result = telemetry_configure_timesync_locked(r);
  if (result != SEDS_OK) {
    printf("Error: failed to configure telemetry timesync: %d\r\n", (int)result);
    seds_router_free(r);
    g_router.r = NULL;
    g_router.created = 0U;
    g_can_side_id = -1;
#ifdef TELEMETRY_BOARD_LINK_UART
    g_board_link_side_id = -1;
#endif
    telemetry_uart_set_side_id(-1);
    return result;
  }

  result = ota_stream_init(r);
  if (result != SEDS_OK) {
    printf("Error: failed to bind OTA stream: %d\r\n", (int)result);
    seds_router_free(r);
    g_router.r = NULL;
    g_router.created = 0U;
    return result;
  }

  /* Discovery begins from the normal poll loop after link startup. */

  g_router.r = r;
  (void)flight_state_cache_init(r);
  g_router.created = 1U;
  g_router.start_time = tx_raw_now_ms_locked();
  return SEDS_OK;
#endif
}

static inline SedsElemKind guess_kind_from_elem_size(size_t elem_size) {
  if (elem_size == 4U || elem_size == 8U) {
    return SEDS_EK_FLOAT;
  }
  return SEDS_EK_UNSIGNED;
}

SedsResult log_telemetry_synchronous(SedsDataType data_type, const void *data,
                                     size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0U || element_size == 0U) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_typed(g_router.r, data_type, data, element_count, element_size,
                               guess_kind_from_elem_size(element_size));
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_asynchronous(SedsDataType data_type, const void *data,
                                      size_t element_count, size_t element_size) {
#ifdef TELEMETRY_ENABLED
  if (!data || element_count == 0U || element_size == 0U) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_queue_typed(g_router.r, data_type, data, element_count, element_size,
                                     guess_kind_from_elem_size(element_size));
#else
  (void)data_type;
  print_data_no_telem((void *)data, element_count * element_size);
  return SEDS_OK;
#endif
}

SedsResult log_telemetry_string_asynchronous(SedsDataType data_type, const char *str) {
#ifdef TELEMETRY_ENABLED
  if (!str) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_log_string_ex(g_router.r, data_type, str, strlen(str), NULL, 1);
#else
  (void)data_type;
  (void)str;
  return SEDS_OK;
#endif
}

SedsResult dispatch_tx_queue(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_tx_queue(g_router.r);
#endif
}

SedsResult process_rx_queue(void) {
#ifndef TELEMETRY_ENABLED
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_rx_queue(g_router.r);
#endif
}

SedsResult dispatch_tx_queue_timeout(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_tx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_rx_queue_timeout(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_rx_queue_with_timeout(g_router.r, timeout_ms);
#endif
}

SedsResult process_all_queues_timeout(uint32_t timeout_ms) {
#ifndef TELEMETRY_ENABLED
  (void)timeout_ms;
  return SEDS_OK;
#else
  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  return seds_router_process_all_queues_with_timeout(g_router.r, timeout_ms);
#endif
}

static SedsResult log_error_impl(uint8_t queue, const char *fmt, va_list args) {
  va_list args_copy;
  int len = 0;
  int written = 0;

  if (!fmt) {
    return SEDS_BAD_ARG;
  }

  if (!g_router.r && init_telemetry_router() != SEDS_OK) {
    return SEDS_ERR;
  }

  va_copy(args_copy, args);
  len = vsnprintf(NULL, 0U, fmt, args_copy);
  va_end(args_copy);

  if (len < 0) {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, empty, 0U, NULL, queue);
  }

  if (len > 512) {
    len = 512;
  }

  char buf[(size_t)len + 1U];
  written = vsnprintf(buf, (size_t)len + 1U, fmt, args);
  if (written < 0) {
    const char *empty = "";
    return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, empty, 0U, NULL, queue);
  }

  return seds_router_log_string_ex(g_router.r, SEDS_DT_TELEMETRY_ERROR, buf, (size_t)written,
                                   NULL, queue);
}

SedsResult log_error_asynchronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(1U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_synchronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(0U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_asyncronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(1U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult log_error_syncronous(const char *fmt, ...) {
#ifndef TELEMETRY_ENABLED
  (void)fmt;
  return SEDS_OK;
#else
  va_list args;
  SedsResult result;

  va_start(args, fmt);
  result = log_error_impl(0U, fmt, args);
  va_end(args);
  return result;
#endif
}

SedsResult print_telemetry_error(const int32_t error_code) {
#ifndef TELEMETRY_ENABLED
  (void)error_code;
  return SEDS_OK;
#else
  const int32_t need = seds_error_to_string_len(error_code);
  if (need <= 0) {
    return (SedsResult)need;
  }

  char buf[(size_t)need];
  SedsResult res = seds_error_to_string(error_code, buf, sizeof(buf));
  if (res == SEDS_OK) {
    printf("Error: %s\r\n", buf);
  } else {
    (void)log_error_asynchronous("Error: seds_error_to_string failed: %d\r\n", (int)res);
  }

  return res;
#endif
}

void die(const char *fmt, ...) {
  char buf[128];
  va_list args;

  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  while (1) {
    printf("FATAL: %s\r\n", buf);
    HAL_Delay(1000);
  }
}
