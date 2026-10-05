#include "board_watchdog.h"
// telemetry_thread.c
#include "GB-Threads.h"
#ifdef TELEMETRY_BOARD_LINK_UART
#include "board_link_uart.h"
#endif
#include "can_bus.h"
#include "main.h"
#include "telemetry.h"
#include "ota_stream.h"
#include "telemetry_uart.h"
#include "tx_api.h"
#include <stdio.h>
#include <string.h>
#define michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael telemetry_uart_process()

extern FDCAN_HandleTypeDef hfdcan2;
#ifdef TELEMETRY_BOARD_LINK_UART
extern UART_HandleTypeDef hlpuart1;
#endif
extern UART_HandleTypeDef huart2;

TX_THREAD telemetry_thread;
TX_THREAD router_test_thread;
#define TELEMETRY_THREAD_STACK_SIZE (13U * 1024U)
#define ROUTER_TEST_THREAD_STACK_SIZE (8U * 1024U)
#define TELEMETRY_QUEUE_SERVICE_BUDGET_MS 1U
#define TELEMETRY_THREAD_SLEEP_TICKS 1U
#define TELEMETRY_CAN_FRAMES_PER_SLICE 8U
#define TELEMETRY_CAN_SERVICE_BUDGET_MS 8U
#define TELEMETRY_CAN_SLICE_BUDGET_MS 2U
#define TELEMETRY_MAINTENANCE_PERIOD_MS 50U

volatile uint32_t g_telemetry_stack_remaining = TELEMETRY_THREAD_STACK_SIZE;
volatile uint32_t g_gateway_telemetry_loop_count = 0U;
/* Milliseconds accumulated in UART, CAN, queues, discovery, time, OTA, sleep. */
volatile uint32_t g_gateway_service_ms[7];
volatile uint32_t g_gateway_service_max_ms[7];
static void service_record(unsigned phase, uint32_t start)
{
    uint32_t elapsed = HAL_GetTick() - start;
    g_gateway_service_ms[phase] += elapsed;
    if (elapsed > g_gateway_service_max_ms[phase]) g_gateway_service_max_ms[phase] = elapsed;
}
#define SERVICE(phase, call) do { uint32_t started = HAL_GetTick(); call; service_record(phase, started); } while (0)


static void sample_telemetry_stack(void)
{
    const volatile uint32_t *const start =
        (const volatile uint32_t *)telemetry_thread.tx_thread_stack_start;
    const volatile uint32_t *const end =
        (const volatile uint32_t *)telemetry_thread.tx_thread_stack_end;
    static const volatile uint32_t *high_water;
    if (start == NULL || end == NULL || start >= end) return;
    if (high_water == NULL || high_water < start || high_water > end) {
        high_water = start;
        while (high_water < end && *high_water == 0xEFEFEFEFUL) ++high_water;
    } else {
        while (high_water > start && high_water[-1] != 0xEFEFEFEFUL) --high_water;
    }
    const uint32_t remaining = (uint32_t)((uintptr_t)high_water -
        (uintptr_t)telemetry_thread.tx_thread_stack_start);
    if (remaining < g_telemetry_stack_remaining)
        g_telemetry_stack_remaining = remaining;
}

void telemetry_thread_entry(ULONG initial_input)
{
    (void)initial_input;

    can_bus_init(&hfdcan2);
    (void)telemetry_uart_init(&huart2);
#ifdef TELEMETRY_BOARD_LINK_UART
    board_link_uart_init(&hlpuart1);
    (void)board_link_uart_start_rx();
#endif
    (void)init_telemetry_router();

    uint32_t maintenance_due = HAL_GetTick();
    for (;;)
    {
        if (can_bus_health_ok())
            board_watchdog_progress(BOARD_WATCHDOG_NETWORK);
        g_gateway_telemetry_loop_count++;
        SERVICE(0, telemetry_uart_process());
        /* Free retained TX payloads before admitting another CAN burst. */
        SERVICE(2, (void)dispatch_tx_queue_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS));
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        /* Drain the live backlog, bounded by wall time rather than 48 frames.
         * UART commands and ACKs run between short CAN slices. Maintenance and
         * the ThreadX yield still run under permanent CAN overload. */
        const uint32_t can_started = HAL_GetTick();
        do {
            uint32_t received;
            SERVICE(1, received = can_bus_process_rx_for(
                TELEMETRY_CAN_FRAMES_PER_SLICE, TELEMETRY_CAN_SLICE_BUDGET_MS));
            SERVICE(0, telemetry_uart_process());
            SERVICE(2, (void)dispatch_tx_queue_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS));
            if (received == 0U || can_bus_rx_pending() == 0U) break;
        } while ((uint32_t)(HAL_GetTick() - can_started) < TELEMETRY_CAN_SERVICE_BUDGET_MS);
        const uint32_t now = HAL_GetTick();
        if ((int32_t)(now - maintenance_due) >= 0) {
            maintenance_due = now + TELEMETRY_MAINTENANCE_PERIOD_MS;
            SERVICE(3, (void)telemetry_poll_discovery());
            SERVICE(4, (void)telemetry_poll_timesync());
        }
        SERVICE(5, ota_stream_poll());
        SERVICE(2, (void)process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS));
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        telemetry_hil_capture_requested_snapshot();
        sample_telemetry_stack();
#ifdef SEDS_FIRMWARE_SIM_TEST
        /* Renode's G491 USART model lacks the board's DMA receive behavior.
         * Service its single-byte RDR shim continuously while still yielding
         * to every other ThreadX thread. */
        tx_thread_relinquish();
#else
        SERVICE(6, tx_thread_sleep(TELEMETRY_THREAD_SLEEP_TICKS));
#endif
    }
}

UINT create_telemetry_thread(TX_BYTE_POOL *byte_pool)
{

    CHAR *pointer;

    /* Allocate the stack for test  */
    if (tx_byte_allocate(byte_pool, (VOID **)&pointer,
                         TELEMETRY_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS)
    {
        return TX_POOL_ERROR;
    }

    UINT status = tx_thread_create(&telemetry_thread,
                                   "Telemetry Thread",
                                   telemetry_thread_entry,
                                   0,
                                   pointer,
                                   TELEMETRY_THREAD_STACK_SIZE,
                                   5,
                                   5,
                                   TX_NO_TIME_SLICE,
                                   TX_AUTO_START);

    return status;
}
