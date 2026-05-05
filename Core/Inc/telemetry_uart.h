#pragma once

#include "sedsprintf.h"
#include "stm32g4xx_hal.h"
#include "tx_api.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TELEMETRY_UART_FRAME_HEADER_SIZE 4U
#define TELEMETRY_UART_MAX_PAYLOAD 256U
#define TELEMETRY_UART_FRAME_SIZE (TELEMETRY_UART_FRAME_HEADER_SIZE + TELEMETRY_UART_MAX_PAYLOAD)
#define TELEMETRY_UART_PAYLOAD_CAPACITY (TELEMETRY_UART_FRAME_SIZE - TELEMETRY_UART_FRAME_HEADER_SIZE)

typedef struct {
  uint32_t rx_byte_count;
  uint32_t rx_frame_count;
  uint32_t rx_data_frame_count;
  uint32_t rx_command_frame_count;
  uint32_t rx_bad_length_count;
  uint32_t rx_bad_padding_count;
  uint32_t rx_reset_count;
  uint32_t rx_dispatch_count;
  uint32_t rx_hw_error_count;
  uint32_t rx_not_ready_count;
  uint32_t rx_idle_line_count;
  uint32_t rx_dma_active;
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
  uint32_t telemetry_deserialize_ok_count;
  uint32_t telemetry_deserialize_fail_count;
  uint32_t tx_frame_count;
} TelemetryUartStats;

SedsResult telemetry_uart_init(UART_HandleTypeDef *huart);
void telemetry_uart_set_byte_pool(TX_BYTE_POOL *pool);
void telemetry_uart_process(void);

SedsResult telemetry_uart_tx_send(const uint8_t *bytes, size_t len, void *user);
void telemetry_uart_send_data_frame(const uint8_t *payload, size_t len);
void telemetry_uart_send_command_frame(const char *text);
void telemetry_uart_reply_next_data_frame(void);
uint32_t telemetry_uart_tx_frame_count(void);
void telemetry_uart_get_stats(TelemetryUartStats *out);
void telemetry_uart_note_deserialize_result(uint8_t success);

void telemetry_uart_set_side_id(int32_t side_id);
int32_t telemetry_uart_side_id(void);

void telemetry_uart_handle_rx_event(UART_HandleTypeDef *huart, uint16_t size);
void telemetry_uart_handle_error(UART_HandleTypeDef *huart);

void telemetry_uart_handle_command(const uint8_t *payload, size_t len);
void telemetry_uart_handle_data(const uint8_t *payload, size_t len);
void telemetry_uart_handle_raw_ascii(const uint8_t *payload, size_t len);

#ifdef __cplusplus
}
#endif
