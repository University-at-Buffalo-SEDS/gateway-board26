#include "telemetry_uart.h"
#include "gateway_status_probe.h"
#include "sim_network_probe.h"

#ifdef TELEMETRY_BOARD_LINK_UART
#include "board_link_uart.h"
#endif
#include "main.h"
#include <string.h>

/* Six full frames absorb a complete constrained-link burst. SEDSNet retains
 * reliable packets when this bounded transport queue applies backpressure;
 * the two removed slots are reassigned to its allocator to preserve a
 * contiguous block for topology/schema updates during long-running traffic. */
#define TELEMETRY_UART_QUEUE_DEPTH 6U
#define TELEMETRY_UART_REQ_DATA_MAGIC 0xA5U
#define TELEMETRY_UART_REQ_COMMAND_MAGIC 0xA6U
#define TELEMETRY_UART_RESP_DATA_MAGIC 0x5AU
#define TELEMETRY_UART_RESP_COMMAND_MAGIC 0x5BU
#define TELEMETRY_UART_REQ_RAW_ASCII_MAGIC 0xA7U
#define TELEMETRY_UART_RESP_RAW_ASCII_MAGIC 0x7AU

#define TELEMETRY_UART_WIRE_MAX_PAYLOAD TELEMETRY_UART_MAX_PAYLOAD
#define TELEMETRY_UART_PICO_UART_MAX_PAYLOAD 4096U
#define TELEMETRY_UART_HEADER_SIZE 4U
#define TELEMETRY_UART_RX_DMA_BUF_SIZE 512U
#define TELEMETRY_UART_RX_RING_DEPTH 8U
#define UNUSED_FUNCTION __attribute__((unused))

typedef struct {
  uint16_t len;
  uint8_t data[TELEMETRY_UART_RX_DMA_BUF_SIZE];
} TelemetryUartRxItem;

typedef struct {
  UART_HandleTypeDef *huart;
  uint8_t rx_dma_buf[TELEMETRY_UART_RX_DMA_BUF_SIZE];
  TelemetryUartRxItem rx_ring[TELEMETRY_UART_RX_RING_DEPTH];
  volatile uint32_t rx_head;
  volatile uint32_t rx_tail;
  volatile uint32_t rx_count;
  uint8_t rx_frame[TELEMETRY_UART_FRAME_SIZE];
  size_t rx_fill;
  size_t rx_expected;
  size_t rx_discard_remaining;
  uint8_t nested_frame[TELEMETRY_UART_FRAME_SIZE];
  size_t nested_fill;
  size_t nested_expected;
  size_t nested_discard_remaining;

  uint8_t tx_payloads[TELEMETRY_UART_QUEUE_DEPTH][TELEMETRY_UART_FRAME_SIZE];
  size_t tx_lengths[TELEMETRY_UART_QUEUE_DEPTH];
  uint8_t tx_head;
  uint8_t tx_tail;
  volatile uint8_t tx_count;
  volatile uint8_t tx_active;
  volatile uint8_t tx_error;
  uint8_t tx_attempts;
  uint32_t tx_started_ms;

  uint32_t tx_frame_count;
  uint8_t rx_dma_active;
  uint32_t rx_dma_start_ok_count;
  uint32_t rx_dma_start_busy_count;
  uint32_t rx_dma_start_error_count;
  uint32_t rx_dma_event_count;
  uint32_t rx_dma_idle_event_count;
  uint32_t rx_dma_tc_event_count;
  uint32_t rx_dma_ht_event_count;
  uint32_t rx_dma_drop_count;
  uint32_t rx_restart_error_count;
  uint32_t rx_dma_last_size;
  uint32_t rx_dma_last_event_type;
  uint32_t rx_dma_last_error_code;
  int32_t side_id;
  TelemetryUartStats stats;
} TelemetryUartState;

static TelemetryUartState g_telemetry_uart = {.side_id = -1};
volatile uint32_t g_gateway_uart_rx_frames = 0U;
volatile uint32_t g_gateway_uart_tx_frames = 0U;
volatile uint32_t g_gateway_uart_tx_failures = 0U;
volatile uint32_t g_gateway_uart_rx_dma_events = 0U;
volatile uint32_t g_gateway_uart_rx_hw_errors = 0U;
volatile uint32_t g_gateway_uart_rx_restarts_failed = 0U;
volatile uint32_t g_gateway_uart_rx_ring_drops = 0U;
#ifdef SEDS_FIRMWARE_SIM_TEST
volatile uint32_t g_sim_uart_rx_irq_bytes = 0U;
volatile uint32_t g_sim_uart_rx_start_ok = 0U;
volatile uint32_t g_sim_uart_rx_start_fail __attribute__((used)) = 0U;
#endif
volatile uint32_t g_gateway_uart_tx_queue_drops = 0U;
volatile uint32_t g_gateway_uart_tx_enqueued = 0U;
volatile uint32_t g_gateway_uart_tx_pending = 0U;
volatile uint32_t g_gateway_uart_tx_high_water = 0U;
volatile uint32_t g_gateway_uart_tx_retry_count = 0U;
volatile uint32_t g_gateway_uart_tx_exhausted = 0U;
volatile uint32_t g_sim_uart_umbilical_status_count = 0U;
#ifdef SEDS_FIRMWARE_SIM_TEST
volatile uint32_t g_sim_uart_umbilical_status_tx_count = 0U;
#endif

void telemetry_uart_set_byte_pool(TX_BYTE_POOL *pool) {
  (void)pool;
}

static uint32_t telemetry_uart_irq_save(void) {
  const uint32_t primask = __get_PRIMASK();
  __disable_irq();
  return primask;
}

static void telemetry_uart_irq_restore(uint32_t primask) {
  if (primask == 0U) {
    __enable_irq();
  }
}

static void telemetry_uart_dispatch_frame(uint8_t magic, const uint8_t *payload,
                                          size_t payload_len);
static void telemetry_uart_dispatch_data_payload(const uint8_t *payload,
                                                 size_t payload_len);
static void telemetry_uart_rx_push_byte(uint8_t byte);
static void telemetry_uart_reset_rx(void);

static HAL_StatusTypeDef telemetry_uart_start_rx_dma(void) {
  HAL_StatusTypeDef status;

  if (g_telemetry_uart.huart == NULL) {
#ifdef SEDS_FIRMWARE_SIM_TEST
    g_sim_uart_rx_start_fail++;
#endif
    return HAL_ERROR;
  }

  __HAL_UART_CLEAR_FLAG(g_telemetry_uart.huart,
                        UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF |
                            UART_CLEAR_OREF | UART_CLEAR_IDLEF);

#ifdef SEDS_FIRMWARE_SIM_TEST
  /* Renode's G491 USART model does not implement the STM32G4 DMA/interrupt
   * receive contract. The simulation service loop drains its modeled RDR;
   * hardware builds continue to use ReceiveToIdle DMA below. */
  status = HAL_OK;
  g_telemetry_uart.rx_dma_active = 1U;
  g_telemetry_uart.rx_dma_start_ok_count++;
  g_sim_uart_rx_start_ok++;
#else
  status = HAL_UARTEx_ReceiveToIdle_DMA(g_telemetry_uart.huart,
                                        g_telemetry_uart.rx_dma_buf,
                                        TELEMETRY_UART_RX_DMA_BUF_SIZE);
  g_telemetry_uart.rx_dma_last_error_code = g_telemetry_uart.huart->ErrorCode;
  if (status == HAL_OK && g_telemetry_uart.huart->hdmarx != NULL) {
    __HAL_DMA_DISABLE_IT(g_telemetry_uart.huart->hdmarx, DMA_IT_HT);
    g_telemetry_uart.rx_dma_active = 1U;
    g_telemetry_uart.rx_dma_start_ok_count++;
  } else {
    g_telemetry_uart.rx_dma_active = 0U;
    if (status == HAL_BUSY) {
      g_telemetry_uart.rx_dma_start_busy_count++;
    } else {
      g_telemetry_uart.rx_dma_start_error_count++;
    }
  }
#endif

  return status;
}

static void telemetry_uart_rx_ring_push_isr(const uint8_t *data, uint16_t len) {
  TelemetryUartRxItem *item;

  if (data == NULL || len == 0U) {
    return;
  }

  if (len > TELEMETRY_UART_RX_DMA_BUF_SIZE) {
    len = TELEMETRY_UART_RX_DMA_BUF_SIZE;
  }

  if (g_telemetry_uart.rx_count >= TELEMETRY_UART_RX_RING_DEPTH) {
    g_telemetry_uart.rx_dma_drop_count++;
    g_gateway_uart_rx_ring_drops++;
    return;
  }

  item = &g_telemetry_uart.rx_ring[g_telemetry_uart.rx_tail];
  item->len = len;
  memcpy(item->data, data, len);
  g_telemetry_uart.rx_tail =
      (g_telemetry_uart.rx_tail + 1U) % TELEMETRY_UART_RX_RING_DEPTH;
  g_telemetry_uart.rx_count++;
}

static uint8_t telemetry_uart_rx_ring_pop(TelemetryUartRxItem *out) {
  uint8_t have = 0U;
  const uint32_t primask = telemetry_uart_irq_save();

  if (out != NULL && g_telemetry_uart.rx_count > 0U) {
    *out = g_telemetry_uart.rx_ring[g_telemetry_uart.rx_head];
    g_telemetry_uart.rx_head =
        (g_telemetry_uart.rx_head + 1U) % TELEMETRY_UART_RX_RING_DEPTH;
    g_telemetry_uart.rx_count--;
    have = 1U;
  }

  telemetry_uart_irq_restore(primask);
  return have;
}

static void telemetry_uart_process_rx_byte(uint8_t byte) {
  telemetry_uart_rx_push_byte(byte);
  g_telemetry_uart.stats.rx_byte_count++;

  if (g_telemetry_uart.rx_expected == 0U ||
      g_telemetry_uart.rx_fill != g_telemetry_uart.rx_expected) {
    return;
  }

  g_telemetry_uart.stats.rx_frame_count++;
  g_gateway_uart_rx_frames++;
  const size_t payload_len =
      (size_t)g_telemetry_uart.rx_frame[2] |
      ((size_t)g_telemetry_uart.rx_frame[3] << 8U);

  if (g_telemetry_uart.rx_frame[0] == TELEMETRY_UART_REQ_DATA_MAGIC) {
    telemetry_uart_dispatch_data_payload(
        &g_telemetry_uart.rx_frame[TELEMETRY_UART_HEADER_SIZE],
        payload_len);
  } else {
    telemetry_uart_dispatch_frame(
        g_telemetry_uart.rx_frame[0],
        &g_telemetry_uart.rx_frame[TELEMETRY_UART_HEADER_SIZE],
        payload_len);
  }

  telemetry_uart_reset_rx();
}

static uint8_t telemetry_uart_is_first_magic(uint8_t byte) {
  return (byte == TELEMETRY_UART_REQ_DATA_MAGIC || byte == TELEMETRY_UART_REQ_COMMAND_MAGIC ||
          byte == TELEMETRY_UART_REQ_RAW_ASCII_MAGIC)
             ? 1U
             : 0U;
}

static uint8_t telemetry_uart_is_valid_header(uint8_t first, uint8_t second) {
  return ((first == TELEMETRY_UART_REQ_DATA_MAGIC && second == TELEMETRY_UART_RESP_DATA_MAGIC) ||
          (first == TELEMETRY_UART_REQ_COMMAND_MAGIC && second == TELEMETRY_UART_RESP_COMMAND_MAGIC) ||
          (first == TELEMETRY_UART_REQ_RAW_ASCII_MAGIC && second == TELEMETRY_UART_RESP_RAW_ASCII_MAGIC))
             ? 1U
             : 0U;
}

static size_t telemetry_uart_clamp_payload_len(size_t len) {
  if (len > TELEMETRY_UART_WIRE_MAX_PAYLOAD) {
    len = TELEMETRY_UART_WIRE_MAX_PAYLOAD;
  }
  if (len > TELEMETRY_UART_MAX_PAYLOAD) {
    len = TELEMETRY_UART_MAX_PAYLOAD;
  }
  return len;
}

static uint8_t telemetry_uart_second_magic(uint8_t magic) {
  switch (magic) {
    case TELEMETRY_UART_REQ_COMMAND_MAGIC:
      return TELEMETRY_UART_RESP_COMMAND_MAGIC;
    case TELEMETRY_UART_REQ_RAW_ASCII_MAGIC:
      return TELEMETRY_UART_RESP_RAW_ASCII_MAGIC;
    case TELEMETRY_UART_REQ_DATA_MAGIC:
    default:
      return TELEMETRY_UART_RESP_DATA_MAGIC;
  }
}

static size_t telemetry_uart_build_frame(uint8_t *frame, uint8_t magic, const uint8_t *payload, size_t len) {
  len = telemetry_uart_clamp_payload_len(len);

  memset(frame, 0, TELEMETRY_UART_FRAME_SIZE);
  frame[0] = magic;
  frame[1] = telemetry_uart_second_magic(magic);
  frame[2] = (uint8_t)(len & 0xFFU);
  frame[3] = (uint8_t)((len >> 8U) & 0xFFU);
  if (payload != NULL && len != 0U) {
    memcpy(&frame[TELEMETRY_UART_HEADER_SIZE], payload, len);
  }
  return TELEMETRY_UART_HEADER_SIZE + len;
}

static void telemetry_uart_reset_rx(void) {
  g_telemetry_uart.stats.rx_reset_count++;
  g_telemetry_uart.rx_fill = 0U;
  g_telemetry_uart.rx_expected = 0U;
  g_telemetry_uart.rx_discard_remaining = 0U;
}

static void telemetry_uart_reset_nested_rx(void) {
  g_telemetry_uart.nested_fill = 0U;
  g_telemetry_uart.nested_expected = 0U;
  g_telemetry_uart.nested_discard_remaining = 0U;
}

static void telemetry_uart_discard_oversize_frame(size_t payload_len) {
  g_telemetry_uart.rx_fill = 0U;
  g_telemetry_uart.rx_expected = 0U;
  g_telemetry_uart.rx_discard_remaining = payload_len;
}

static void telemetry_uart_discard_oversize_nested_frame(size_t payload_len) {
  g_telemetry_uart.nested_fill = 0U;
  g_telemetry_uart.nested_expected = 0U;
  g_telemetry_uart.nested_discard_remaining = payload_len;
}

static void telemetry_uart_signal_parse_failure(void) {
  (void)0;
}

static void telemetry_uart_dispatch_frame(uint8_t magic, const uint8_t *payload, size_t payload_len) {
  switch (magic) {
    case TELEMETRY_UART_REQ_DATA_MAGIC:
      g_telemetry_uart.stats.rx_data_frame_count++;
      g_telemetry_uart.stats.rx_dispatch_count++;
      telemetry_uart_handle_data(payload, payload_len);
      break;

    case TELEMETRY_UART_REQ_COMMAND_MAGIC:
      g_telemetry_uart.stats.rx_command_frame_count++;
      g_telemetry_uart.stats.rx_dispatch_count++;
      telemetry_uart_handle_command(payload, payload_len);
      break;

    case TELEMETRY_UART_REQ_RAW_ASCII_MAGIC:
      g_telemetry_uart.stats.rx_command_frame_count++;
      g_telemetry_uart.stats.rx_dispatch_count++;
      telemetry_uart_handle_raw_ascii(payload, payload_len);
      break;

    default:
      break;
  }
}

static void telemetry_uart_nested_rx_push_byte(uint8_t byte) {
  if (g_telemetry_uart.nested_discard_remaining != 0U) {
    g_telemetry_uart.nested_discard_remaining--;
    return;
  }

  if (g_telemetry_uart.nested_fill == 0U) {
    if (!telemetry_uart_is_first_magic(byte)) {
      return;
    }
    g_telemetry_uart.nested_frame[0] = byte;
    g_telemetry_uart.nested_fill = 1U;
    return;
  }

  if (g_telemetry_uart.nested_fill == 1U) {
    g_telemetry_uart.nested_frame[1] = byte;
    if (!telemetry_uart_is_valid_header(g_telemetry_uart.nested_frame[0], byte)) {
      g_telemetry_uart.stats.rx_bad_length_count++;
      telemetry_uart_signal_parse_failure();
      if (telemetry_uart_is_first_magic(byte)) {
        g_telemetry_uart.nested_frame[0] = byte;
        g_telemetry_uart.nested_fill = 1U;
      } else {
        telemetry_uart_reset_nested_rx();
      }
      return;
    }
    g_telemetry_uart.nested_fill = 2U;
    return;
  }

  if (g_telemetry_uart.nested_fill == 3U) {
    const size_t payload_len =
        (size_t)g_telemetry_uart.nested_frame[2] | ((size_t)byte << 8U);
    g_telemetry_uart.nested_frame[3] = byte;
    if (payload_len > TELEMETRY_UART_WIRE_MAX_PAYLOAD) {
      g_telemetry_uart.stats.rx_bad_length_count++;
      telemetry_uart_signal_parse_failure();
      if (payload_len <= TELEMETRY_UART_PICO_UART_MAX_PAYLOAD) {
        telemetry_uart_discard_oversize_nested_frame(payload_len);
      } else {
        telemetry_uart_reset_nested_rx();
      }
      return;
    }
    g_telemetry_uart.nested_expected = TELEMETRY_UART_HEADER_SIZE + payload_len;
    g_telemetry_uart.nested_fill = 4U;
    return;
  }

  if (g_telemetry_uart.nested_fill < TELEMETRY_UART_FRAME_SIZE) {
    g_telemetry_uart.nested_frame[g_telemetry_uart.nested_fill++] = byte;
    if (g_telemetry_uart.nested_expected != 0U &&
        g_telemetry_uart.nested_fill == g_telemetry_uart.nested_expected) {
      const size_t payload_len =
          (size_t)g_telemetry_uart.nested_frame[2] |
          ((size_t)g_telemetry_uart.nested_frame[3] << 8U);
      g_telemetry_uart.stats.rx_frame_count++;
      telemetry_uart_dispatch_frame(
          g_telemetry_uart.nested_frame[0],
          &g_telemetry_uart.nested_frame[TELEMETRY_UART_HEADER_SIZE],
          payload_len);
      telemetry_uart_reset_nested_rx();
    }
    return;
  }

  telemetry_uart_signal_parse_failure();
  telemetry_uart_reset_nested_rx();
}

static void telemetry_uart_dispatch_data_payload(const uint8_t *payload, size_t payload_len) {
  size_t idx;
  const uint8_t is_sedsnet_side_transport =
      (payload_len >= 3U && payload[0] == (uint8_t)'S' &&
       payload[1] == (uint8_t)'D' && payload[2] == (uint8_t)'T');

  if (g_telemetry_uart.nested_fill == 0U &&
      g_telemetry_uart.nested_discard_remaining == 0U &&
      (payload_len < 2U || !telemetry_uart_is_valid_header(payload[0], payload[1]))) {
    /* A profiled SEDSNet side sends SDT full/compact envelopes, not canonical
     * packets.  Pass those envelopes to the side-aware router decoder; only
     * apply canonical validation to the legacy unprofiled payload form. */
    if (is_sedsnet_side_transport == 0U &&
        seds_pkt_validate_packed(payload, payload_len) != SEDS_OK) {
      telemetry_uart_signal_parse_failure();
      return;
    }
    telemetry_uart_dispatch_frame(TELEMETRY_UART_REQ_DATA_MAGIC, payload, payload_len);
    return;
  }

  for (idx = 0U; idx < payload_len; ++idx) {
    telemetry_uart_nested_rx_push_byte(payload[idx]);
  }
}

static void telemetry_uart_rx_push_byte(uint8_t byte) {
  if (g_telemetry_uart.rx_discard_remaining != 0U) {
    g_telemetry_uart.rx_discard_remaining--;
    return;
  }

  if (g_telemetry_uart.rx_fill == 0U) {
    if (!telemetry_uart_is_first_magic(byte)) {
      return;
    }
    g_telemetry_uart.rx_frame[0] = byte;
    g_telemetry_uart.rx_fill = 1U;
    return;
  }

  if (g_telemetry_uart.rx_fill == 1U) {
    g_telemetry_uart.rx_frame[1] = byte;
    if (!telemetry_uart_is_valid_header(g_telemetry_uart.rx_frame[0], byte)) {
      g_telemetry_uart.stats.rx_bad_length_count++;
      telemetry_uart_signal_parse_failure();
      if (telemetry_uart_is_first_magic(byte)) {
        g_telemetry_uart.rx_frame[0] = byte;
        g_telemetry_uart.rx_fill = 1U;
      } else {
        telemetry_uart_reset_rx();
      }
      return;
    }
    g_telemetry_uart.rx_fill = 2U;
    return;
  }

  if (g_telemetry_uart.rx_fill == 3U) {
    const size_t payload_len =
        (size_t)g_telemetry_uart.rx_frame[2] | ((size_t)byte << 8U);
    g_telemetry_uart.rx_frame[3] = byte;
    if (payload_len > TELEMETRY_UART_WIRE_MAX_PAYLOAD) {
      g_telemetry_uart.stats.rx_bad_length_count++;
      telemetry_uart_signal_parse_failure();
      if (payload_len <= TELEMETRY_UART_PICO_UART_MAX_PAYLOAD) {
        telemetry_uart_discard_oversize_frame(payload_len);
      } else {
        telemetry_uart_reset_rx();
      }
      return;
    }

    g_telemetry_uart.rx_expected = TELEMETRY_UART_HEADER_SIZE + payload_len;
    g_telemetry_uart.rx_fill = 4U;
    return;
  }

  if (g_telemetry_uart.rx_fill < TELEMETRY_UART_FRAME_SIZE) {
    g_telemetry_uart.rx_frame[g_telemetry_uart.rx_fill++] = byte;
    return;
  }

  telemetry_uart_signal_parse_failure();
  telemetry_uart_reset_rx();
}

/* Called with IRQs masked. DMA owns the head slot until UART TC, not
 * merely DMA transfer-complete. No router APIs or allocation in this path. */
static void telemetry_uart_tx_kick_locked(void) {
  if (g_telemetry_uart.tx_active || g_telemetry_uart.tx_error ||
      !g_telemetry_uart.tx_count || !g_telemetry_uart.huart) return;
  const unsigned slot = g_telemetry_uart.tx_head;
  g_telemetry_uart.tx_started_ms = HAL_GetTick();
  g_telemetry_uart.tx_active = 1U;
  const HAL_StatusTypeDef status = HAL_UART_Transmit_DMA(
      g_telemetry_uart.huart, g_telemetry_uart.tx_payloads[slot],
      (uint16_t)g_telemetry_uart.tx_lengths[slot]);
  if (status != HAL_OK) {
    g_telemetry_uart.tx_active = 0U;
    g_telemetry_uart.tx_error = 1U;
  }
}

static uint8_t telemetry_uart_enqueue_frame(uint8_t magic,
                                            const uint8_t *payload, size_t len) {
  if (len > TELEMETRY_UART_PAYLOAD_CAPACITY || (len && !payload)) return 0U;
  const uint32_t primask = telemetry_uart_irq_save();
  if (g_telemetry_uart.tx_count == TELEMETRY_UART_QUEUE_DEPTH) {
    g_gateway_uart_tx_queue_drops++; /* Rejected; router retains reliable data. */
    telemetry_uart_irq_restore(primask);
    return 0U;
  }
  const unsigned slot = g_telemetry_uart.tx_tail;
  g_telemetry_uart.tx_lengths[slot] = telemetry_uart_build_frame(
      g_telemetry_uart.tx_payloads[slot], magic, payload, len);
  g_telemetry_uart.tx_tail = (slot + 1U) % TELEMETRY_UART_QUEUE_DEPTH;
  g_telemetry_uart.tx_count++;
  g_gateway_uart_tx_enqueued++;
  g_gateway_uart_tx_pending = g_telemetry_uart.tx_count;
  if (g_gateway_uart_tx_pending > g_gateway_uart_tx_high_water)
    g_gateway_uart_tx_high_water = g_gateway_uart_tx_pending;
  telemetry_uart_tx_kick_locked();
  telemetry_uart_irq_restore(primask);
  return 1U;
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
  if (huart != g_telemetry_uart.huart) return;
  const uint32_t primask = telemetry_uart_irq_save();
  if (g_telemetry_uart.tx_active && !g_telemetry_uart.tx_error) {
    const unsigned slot = g_telemetry_uart.tx_head;
    gateway_status_observe(GW_STATUS_UART_SENT,
        g_telemetry_uart.tx_payloads[slot] + TELEMETRY_UART_HEADER_SIZE,
        g_telemetry_uart.tx_lengths[slot] - TELEMETRY_UART_HEADER_SIZE,
        SEDS_DT_UMBILICAL_STATUS, tx_time_get());
#ifdef SEDS_FIRMWARE_SIM_TEST
    if (sim_probe_packed_data_type(g_telemetry_uart.tx_payloads[slot] + TELEMETRY_UART_HEADER_SIZE)
        == (uint32_t)SEDS_DT_UMBILICAL_STATUS) g_sim_uart_umbilical_status_count++;
#endif
    g_telemetry_uart.tx_active = 0U;
    g_telemetry_uart.tx_head = (g_telemetry_uart.tx_head + 1U) % TELEMETRY_UART_QUEUE_DEPTH;
    g_telemetry_uart.tx_count--;
    g_telemetry_uart.tx_attempts = 0U;
    g_gateway_uart_tx_pending = g_telemetry_uart.tx_count;
    g_telemetry_uart.tx_frame_count++;
    g_gateway_uart_tx_frames++;
    /* Chain immediately: no one-frame-per-thread-wakeup ceiling. */
    telemetry_uart_tx_kick_locked();
  }
  telemetry_uart_irq_restore(primask);
}

static void telemetry_uart_flush_tx_queue(void) {
  uint32_t primask = telemetry_uart_irq_save();
  if (g_telemetry_uart.tx_count) {
    const unsigned slot = g_telemetry_uart.tx_head;
    const uint32_t baud = g_telemetry_uart.huart->Init.BaudRate;
    const uint32_t wire_ms = baud ?
        (g_telemetry_uart.tx_lengths[slot] * 10000U + baud - 1U) / baud : 100U;
    if (g_telemetry_uart.tx_error ||
        (g_telemetry_uart.tx_active &&
         (uint32_t)(HAL_GetTick() - g_telemetry_uart.tx_started_ms) > wire_ms + 50U)) {
      /* Synchronous abort disables DMA before the slot can be reused. It does
       * not wait for wire transmission. Retry at most twice, then count loss.
       * A partial frame may also invalidate a retry at the peer; wire CRC and
       * SEDSNet reliability remain responsible for end-to-end recovery. */
      g_telemetry_uart.tx_error = 1U; /* Completion must not release the slot. */
      telemetry_uart_irq_restore(primask);
      const HAL_StatusTypeDef abort_status = HAL_UART_AbortTransmit(g_telemetry_uart.huart);
      primask = telemetry_uart_irq_save();
      if (abort_status != HAL_OK) {
        /* DMA ownership is uncertain: retain the buffer and retry abort next
         * service. Never recycle memory that hardware may still be reading. */
        g_gateway_uart_tx_failures++;
        telemetry_uart_irq_restore(primask);
        return;
      }
      g_telemetry_uart.tx_active = 0U;
      g_telemetry_uart.tx_error = 0U;
      g_gateway_uart_tx_failures++;
      gateway_status_observe(GW_STATUS_UART_FAILED,
          g_telemetry_uart.tx_payloads[slot] + TELEMETRY_UART_HEADER_SIZE,
          g_telemetry_uart.tx_lengths[slot] - TELEMETRY_UART_HEADER_SIZE,
          SEDS_DT_UMBILICAL_STATUS, tx_time_get());
      if (++g_telemetry_uart.tx_attempts >= 3U) {
        g_gateway_uart_tx_exhausted++;
        g_telemetry_uart.tx_head = (slot + 1U) % TELEMETRY_UART_QUEUE_DEPTH;
        g_telemetry_uart.tx_count--;
        g_telemetry_uart.tx_attempts = 0U;
        g_gateway_uart_tx_pending = g_telemetry_uart.tx_count;
      } else g_gateway_uart_tx_retry_count++;
    }
  }
  telemetry_uart_tx_kick_locked();
  telemetry_uart_irq_restore(primask);
}

SedsResult telemetry_uart_init(UART_HandleTypeDef *huart) {
  if (huart == NULL) {
    return SEDS_BAD_ARG;
  }

  memset(&g_telemetry_uart, 0, sizeof(g_telemetry_uart));
  g_telemetry_uart.huart = huart;
  g_telemetry_uart.side_id = -1;
  HAL_GPIO_WritePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin, GPIO_PIN_RESET);

  (void)telemetry_uart_start_rx_dma();
  return SEDS_OK;
}

void telemetry_uart_process(void) {
  TelemetryUartRxItem item;

#ifdef SEDS_FIRMWARE_SIM_TEST
  while (__HAL_UART_GET_FLAG(g_telemetry_uart.huart, UART_FLAG_RXNE) != RESET) {
    const uint8_t byte = (uint8_t)READ_REG(g_telemetry_uart.huart->Instance->RDR);
    g_sim_uart_rx_irq_bytes++;
    telemetry_uart_process_rx_byte(byte);
  }
#endif

  while (telemetry_uart_rx_ring_pop(&item)) {
    for (size_t idx = 0U; idx < (size_t)item.len; ++idx) {
      telemetry_uart_process_rx_byte(item.data[idx]);
    }
  }

  if (g_telemetry_uart.rx_dma_active == 0U) {
    (void)telemetry_uart_start_rx_dma();
  }

  telemetry_uart_flush_tx_queue();
}

SedsResult telemetry_uart_tx_send(const uint8_t *bytes, size_t len, void *user) {
#ifdef SEDS_FIRMWARE_SIM_TEST
  extern volatile uint32_t g_sim_uart_egress_peer_mask;
#endif
  (void)user;

  if (bytes == NULL || len == 0U) {
    return SEDS_BAD_ARG;
  }

#ifdef SEDS_FIRMWARE_SIM_TEST
  g_sim_uart_egress_peer_mask |= sim_probe_peer_bit_packed(bytes, len);
  if (sim_probe_packed_data_type(bytes, len) ==
      (uint32_t)SEDS_DT_UMBILICAL_STATUS) {
    g_sim_uart_umbilical_status_tx_count++;
  }
#endif

  if (!telemetry_uart_enqueue_frame(TELEMETRY_UART_REQ_DATA_MAGIC, bytes, len)) return SEDS_IO;
  const uint32_t primask = telemetry_uart_irq_save();
  gateway_status_observe(GW_STATUS_UART_QUEUED, bytes, len,
                         SEDS_DT_UMBILICAL_STATUS, tx_time_get());
  telemetry_uart_irq_restore(primask);
  return SEDS_OK;
}

void telemetry_uart_send_data_frame(const uint8_t *payload, size_t len) {
  (void)telemetry_uart_enqueue_frame(TELEMETRY_UART_REQ_DATA_MAGIC, payload, len);
}

void telemetry_uart_send_command_frame(const char *text) {
  uint8_t payload[TELEMETRY_UART_PAYLOAD_CAPACITY];
  size_t len = 0U;

  if (text != NULL) {
    len = strlen(text);
    len = telemetry_uart_clamp_payload_len(len + 1U);

    if (len != 0U) {
      size_t text_len = len - 1U;
      if (text_len != 0U) {
        memcpy(payload, text, text_len);
      }
      payload[text_len] = '\n';
      (void)telemetry_uart_enqueue_frame(TELEMETRY_UART_REQ_COMMAND_MAGIC, payload, len);
      return;
    }
  }

  (void)telemetry_uart_enqueue_frame(TELEMETRY_UART_REQ_COMMAND_MAGIC, NULL, 0U);
}

void telemetry_uart_reply_next_data_frame(void) {
  /* Compatibility poll: data is already streamed by DMA when queued. */
  telemetry_uart_flush_tx_queue();
}

uint32_t telemetry_uart_tx_frame_count(void) {
  return g_telemetry_uart.tx_frame_count;
}

void telemetry_uart_get_stats(TelemetryUartStats *out) {
  if (out == NULL) {
    return;
  }

  *out = g_telemetry_uart.stats;
  out->tx_frame_count = g_telemetry_uart.tx_frame_count;
  out->tx_enqueued = g_gateway_uart_tx_enqueued;
  out->tx_pending = g_gateway_uart_tx_pending;
  out->tx_high_water = g_gateway_uart_tx_high_water;
  out->tx_rejected = g_gateway_uart_tx_queue_drops;
  out->tx_retries = g_gateway_uart_tx_retry_count;
  out->tx_exhausted = g_gateway_uart_tx_exhausted;
  out->rx_dma_active = g_telemetry_uart.rx_dma_active;
  out->rx_dma_start_ok_count = g_telemetry_uart.rx_dma_start_ok_count;
  out->rx_dma_start_busy_count = g_telemetry_uart.rx_dma_start_busy_count;
  out->rx_dma_start_error_count = g_telemetry_uart.rx_dma_start_error_count;
  out->rx_dma_event_count = g_telemetry_uart.rx_dma_event_count;
  out->rx_dma_idle_event_count = g_telemetry_uart.rx_dma_idle_event_count;
  out->rx_dma_tc_event_count = g_telemetry_uart.rx_dma_tc_event_count;
  out->rx_dma_ht_event_count = g_telemetry_uart.rx_dma_ht_event_count;
  out->rx_dma_drop_count = g_telemetry_uart.rx_dma_drop_count;
  out->rx_restart_error_count = g_telemetry_uart.rx_restart_error_count;
  out->rx_dma_last_size = g_telemetry_uart.rx_dma_last_size;
  out->rx_dma_last_event_type = g_telemetry_uart.rx_dma_last_event_type;
  out->rx_dma_last_error_code = g_telemetry_uart.rx_dma_last_error_code;
}

void telemetry_uart_note_deserialize_result(uint8_t success) {
  if (success != 0U) {
    g_telemetry_uart.stats.telemetry_deserialize_ok_count++;
  } else {
    g_telemetry_uart.stats.telemetry_deserialize_fail_count++;
  }
}

void telemetry_uart_set_side_id(int32_t side_id) {
  g_telemetry_uart.side_id = side_id;
}

int32_t telemetry_uart_side_id(void) {
  return g_telemetry_uart.side_id;
}

void telemetry_uart_handle_rx_event(UART_HandleTypeDef *huart, uint16_t size) {
  if (g_telemetry_uart.huart == NULL || huart == NULL ||
      huart->Instance != g_telemetry_uart.huart->Instance) {
    return;
  }

  const HAL_UART_RxEventTypeTypeDef event_type = HAL_UARTEx_GetRxEventType(huart);
  g_telemetry_uart.rx_dma_event_count++;
  g_gateway_uart_rx_dma_events++;
#ifdef SEDS_FIRMWARE_SIM_TEST
  g_sim_uart_rx_irq_bytes++;
#endif
  g_telemetry_uart.rx_dma_last_size = size;
  g_telemetry_uart.rx_dma_last_event_type = event_type;
  g_telemetry_uart.rx_dma_last_error_code = huart->ErrorCode;
  if (event_type == HAL_UART_RXEVENT_IDLE) {
    g_telemetry_uart.rx_dma_idle_event_count++;
  } else if (event_type == HAL_UART_RXEVENT_TC) {
    g_telemetry_uart.rx_dma_tc_event_count++;
  } else if (event_type == HAL_UART_RXEVENT_HT) {
    g_telemetry_uart.rx_dma_ht_event_count++;
  }

  telemetry_uart_rx_ring_push_isr(g_telemetry_uart.rx_dma_buf, size);
  if (telemetry_uart_start_rx_dma() != HAL_OK) {
    g_telemetry_uart.rx_dma_active = 0U;
    g_telemetry_uart.rx_restart_error_count++;
    g_gateway_uart_rx_restarts_failed++;
  }
}

void telemetry_uart_handle_error(UART_HandleTypeDef *huart) {
  if (g_telemetry_uart.huart == NULL || huart == NULL ||
      huart->Instance != g_telemetry_uart.huart->Instance) {
    return;
  }

  if (huart->ErrorCode & HAL_UART_ERROR_DMA) g_telemetry_uart.tx_error = 1U;
  g_telemetry_uart.stats.rx_hw_error_count++;
  g_gateway_uart_rx_hw_errors++;
  g_telemetry_uart.rx_dma_last_error_code = huart->ErrorCode;
  (void)HAL_UART_AbortReceive(huart);
  if (telemetry_uart_start_rx_dma() != HAL_OK) {
    g_telemetry_uart.rx_dma_active = 0U;
    g_telemetry_uart.rx_restart_error_count++;
    g_gateway_uart_rx_restarts_failed++;
  }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size) {
  telemetry_uart_handle_rx_event(huart, Size);
#ifdef TELEMETRY_BOARD_LINK_UART
  board_link_uart_handle_rx_event(huart, Size);
#endif
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
  telemetry_uart_handle_error(huart);
#ifdef TELEMETRY_BOARD_LINK_UART
  board_link_uart_handle_error(huart);
#endif
}
