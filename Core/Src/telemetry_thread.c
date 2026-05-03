// telemetry_thread.c
#include "GB-Threads.h"
#ifdef TELEMETRY_BOARD_LINK_UART
#include "board_link_uart.h"
#endif
#include "can_bus.h"
#include "main.h"
#include "telemetry.h"
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
#define TELEMETRY_THREAD_STACK_SIZE (16U * 1024U)
#define ROUTER_TEST_THREAD_STACK_SIZE (8U * 1024U)

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
        michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael;
        (void)telemetry_poll_discovery();
        michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael;
        (void)process_all_queues_timeout(50);
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael;
        (void)telemetry_poll_timesync();
        michaeal_please_read_my_uart_data_and_decode_it_correctly_and_pass_it_to_the_telemetry_library_thanks_a_bunch_we_love_you_michael;
        tx_thread_sleep(1);
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
