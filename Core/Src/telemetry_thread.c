// telemetry_thread.c
#include "GB-Threads.h"
#include "can_bus.h"
#include "main.h"
#include "telemetry.h"
#include "telemetry_uart.h"
#include "tx_api.h"
#include <stdio.h>
#include <string.h>

extern FDCAN_HandleTypeDef hfdcan2;
extern UART_HandleTypeDef huart2;

TX_THREAD telemetry_thread;
TX_THREAD router_test_thread;
#define TELEMETRY_THREAD_STACK_SIZE (16U *1024U)
#define ROUTER_TEST_THREAD_STACK_SIZE (8U * 1024U)
#define BATTERY_VOLTAGE_REPORT_VALUE 14.01f
#define BATTERY_VOLTAGE_REPORT_PERIOD_TICKS ((ULONG)TX_TIMER_TICKS_PER_SECOND)

static void telemetry_report_battery_voltage_periodic(void)
{
    static ULONG last_report_ticks = 0U;
    static uint8_t report_started = 0U;
    const ULONG now_ticks = tx_time_get();

    if (report_started != 0U &&
        (ULONG)(now_ticks - last_report_ticks) < BATTERY_VOLTAGE_REPORT_PERIOD_TICKS) {
        return;
    }

    const float battery_voltage = BATTERY_VOLTAGE_REPORT_VALUE;
    last_report_ticks = now_ticks;
    report_started = 1U;
    (void)log_telemetry_asynchronous(SEDS_DT_BATTERY_VOLTAGE, &battery_voltage, 1U,
                                     sizeof(battery_voltage));
}

void telemetry_thread_entry(ULONG initial_input)
{
    (void)initial_input;

    can_bus_init(&hfdcan2);
    (void)telemetry_uart_init(&huart2);
    (void)init_telemetry_router();

    for (;;) {
        telemetry_uart_process();
        can_bus_process_rx();
        telemetry_uart_process();
        telemetry_report_battery_voltage_periodic();
        telemetry_uart_process();
        (void)telemetry_poll_discovery();
        telemetry_uart_process();
        (void)process_all_queues_timeout(0);
        telemetry_uart_process();
        (void)telemetry_poll_timesync();
        telemetry_uart_process();
        tx_thread_relinquish();
    }
}

UINT create_telemetry_thread(TX_BYTE_POOL *byte_pool)
{

        CHAR *pointer;

  /* Allocate the stack for test  */
  if (tx_byte_allocate(byte_pool, (VOID**) &pointer,
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
