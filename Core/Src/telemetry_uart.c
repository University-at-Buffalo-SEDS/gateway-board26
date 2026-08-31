#include "telemetry_uart.h"

#ifdef TELEMETRY_BOARD_LINK_UART
#include "board_link_uart.h"
#endif
#include "main.h"
#include <string.h>

#define TELEMETRY_UART_QUEUE_DEPTH 8U
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

  uint8_t tx_payloads[TELEMETRY_UART_QUEUE_DEPTH][TELEMETRY_UART_PAYLOAD_CAPACITY];
  size_t tx_lengths[TELEMETRY_UART_QUEUE_DEPTH];
  uint8_t tx_head;
  uint8_t tx_tail;
  uint8_t tx_count;

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
    return HAL_ERROR;
  }

  __HAL_UART_CLEAR_FLAG(g_telemetry_uart.huart,
                        UART_CLEAR_PEF | UART_CLEAR_FEF | UART_CLEAR_NEF |
                            UART_CLEAR_OREF | UART_CLEAR_IDLEF);

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

  if (g_telemetry_uart.nested_fill == 0U &&
      g_telemetry_uart.nested_discard_remaining == 0U &&
      (payload_len < 2U || !telemetry_uart_is_valid_header(payload[0], payload[1]))) {
    if (seds_pkt_validate_packed(payload, payload_len) != SEDS_OK) {
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

static void telemetry_uart_write_frame(uint8_t magic, const uint8_t *payload, size_t len) {
  uint8_t frame[TELEMETRY_UART_FRAME_SIZE];
  size_t frame_len = 0U;

  if (g_telemetry_uart.huart == NULL) {
    return;
  }

  len = telemetry_uart_clamp_payload_len(len);
  frame_len = telemetry_uart_build_frame(frame, magic, payload, len);
  (void)HAL_UART_Transmit(g_telemetry_uart.huart, frame, (uint16_t)frame_len, 100U);
  g_telemetry_uart.tx_frame_count++;
}

static UNUSED_FUNCTION uint8_t telemetry_uart_queue_push(const uint8_t *bytes, size_t len) {
  uint32_t primask;
  uint8_t slot;

  if (bytes == NULL || len == 0U) {
    return 0U;
  }

  len = telemetry_uart_clamp_payload_len(len);
  if (len == 0U) {
    return 0U;
  }

  primask = telemetry_uart_irq_save();

  if (g_telemetry_uart.tx_count >= TELEMETRY_UART_QUEUE_DEPTH) {
    g_telemetry_uart.tx_head = (uint8_t)((g_telemetry_uart.tx_head + 1U) % TELEMETRY_UART_QUEUE_DEPTH);
    g_telemetry_uart.tx_count--;
  }

  slot = g_telemetry_uart.tx_tail;
  memcpy(g_telemetry_uart.tx_payloads[slot], bytes, len);
  g_telemetry_uart.tx_lengths[slot] = len;
  g_telemetry_uart.tx_tail = (uint8_t)((slot + 1U) % TELEMETRY_UART_QUEUE_DEPTH);
  g_telemetry_uart.tx_count++;

  telemetry_uart_irq_restore(primask);
  return 1U;
}

static size_t telemetry_uart_queue_pop(uint8_t *out) {
  uint32_t primask;
  uint8_t slot;
  size_t len;

  if (out == NULL) {
    return 0U;
  }

  primask = telemetry_uart_irq_save();

  if (g_telemetry_uart.tx_count == 0U) {
    telemetry_uart_irq_restore(primask);
    return 0U;
  }

  slot = g_telemetry_uart.tx_head;
  len = g_telemetry_uart.tx_lengths[slot];
  memcpy(out, g_telemetry_uart.tx_payloads[slot], len);

  g_telemetry_uart.tx_head = (uint8_t)((slot + 1U) % TELEMETRY_UART_QUEUE_DEPTH);
  g_telemetry_uart.tx_count--;

  telemetry_uart_irq_restore(primask);
  return len;
}

static void telemetry_uart_flush_tx_queue(void) {
  while (g_telemetry_uart.tx_count != 0U) {
    telemetry_uart_reply_next_data_frame();
  }
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
  (void)user;

  if (bytes == NULL || len == 0U) {
    return SEDS_BAD_ARG;
  }

  telemetry_uart_send_data_frame(bytes, len);
  return SEDS_OK;
}

void telemetry_uart_send_data_frame(const uint8_t *payload, size_t len) {
  telemetry_uart_write_frame(TELEMETRY_UART_REQ_DATA_MAGIC, payload, len);
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
      telemetry_uart_write_frame(TELEMETRY_UART_REQ_COMMAND_MAGIC, payload, len);
      return;
    }
  }

  telemetry_uart_write_frame(TELEMETRY_UART_REQ_COMMAND_MAGIC, NULL, 0U);
}

void telemetry_uart_reply_next_data_frame(void) {
  uint8_t payload[TELEMETRY_UART_PAYLOAD_CAPACITY];
  const size_t len = telemetry_uart_queue_pop(payload);
  telemetry_uart_send_data_frame(payload, len);
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
  }
}

void telemetry_uart_handle_error(UART_HandleTypeDef *huart) {
  if (g_telemetry_uart.huart == NULL || huart == NULL ||
      huart->Instance != g_telemetry_uart.huart->Instance) {
    return;
  }

  g_telemetry_uart.stats.rx_hw_error_count++;
  g_telemetry_uart.rx_dma_last_error_code = huart->ErrorCode;
  (void)HAL_UART_AbortReceive(huart);
  if (telemetry_uart_start_rx_dma() != HAL_OK) {
    g_telemetry_uart.rx_dma_active = 0U;
    g_telemetry_uart.rx_restart_error_count++;
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
