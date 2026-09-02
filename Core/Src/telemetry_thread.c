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
#define TELEMETRY_THREAD_STACK_SIZE (12U * 1024U)
#define ROUTER_TEST_THREAD_STACK_SIZE (8U * 1024U)
#define TELEMETRY_QUEUE_SERVICE_BUDGET_MS 5U
#define TELEMETRY_THREAD_SLEEP_TICKS 1U

volatile uint32_t g_telemetry_stack_remaining = TELEMETRY_THREAD_STACK_SIZE;

static void sample_telemetry_stack(void)
{
    const uint32_t *cursor = (const uint32_t *)telemetry_thread.tx_thread_stack_start;
    const uint32_t *const end = (const uint32_t *)telemetry_thread.tx_thread_stack_end;
    if (cursor == NULL || end == NULL || cursor >= end) return;
    while (cursor < end && *cursor == 0xEFEFEFEFUL) ++cursor;
    const uint32_t remaining = (uint32_t)((uintptr_t)cursor -
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

    for (;;)
    {
        michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael;
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        can_bus_process_rx();
        (void)telemetry_poll_discovery();
        (void)telemetry_poll_timesync();
        ota_stream_poll();
        (void)process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS);
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        (void)telemetry_poll_timesync();
        sample_telemetry_stack();
        tx_thread_sleep(TELEMETRY_THREAD_SLEEP_TICKS);
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
