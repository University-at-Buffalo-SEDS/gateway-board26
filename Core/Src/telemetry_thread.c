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
static TX_SEMAPHORE telemetry_rx_activity;
static uint8_t telemetry_immediate_wakes;

static void telemetry_notify_rx_activity(void) {
    /* Coalesce a burst into one pending wake. ThreadX supports this in ISR
     * context; all packet ownership/dispatch remains in the worker. */
    (void)tx_semaphore_ceiling_put(&telemetry_rx_activity, 1U);
}

static void telemetry_wait_for_activity(void) {
    /* A continuous source must not starve the lower-priority sensor thread.
     * At most four immediate wakes occur before one mandatory scheduler tick. */
    if (telemetry_immediate_wakes >= 4U) {
        telemetry_immediate_wakes = 0U;
        (void)tx_thread_sleep(1U);
        return;
    }
    if (tx_semaphore_get(&telemetry_rx_activity, 1U) == TX_SUCCESS)
        ++telemetry_immediate_wakes;
    else
        telemetry_immediate_wakes = 0U;
}
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

    (void)telemetry_uart_init(&huart2);
#ifdef TELEMETRY_BOARD_LINK_UART
    board_link_uart_init(&hlpuart1);
    (void)board_link_uart_start_rx();
#endif
    (void)init_telemetry_router();
    /* Start CAN only after its network consumer is ready. Router construction
     * cannot service the receive ring; starting first overflows it at boot. */
    can_bus_set_rx_wakeup_callback(telemetry_notify_rx_activity);
    can_bus_init(&hfdcan2);

    uint32_t maintenance_due = HAL_GetTick();
    for (;;)
    {
        if (can_bus_health_ok())
            board_watchdog_progress(BOARD_WATCHDOG_NETWORK);
        g_gateway_telemetry_loop_count++;
        SERVICE(0, telemetry_uart_process());
        /* Free retained TX payloads on a quiet receive path. Encoding one
         * queued discovery frame can overrun its requested time budget; do
         * not place that work in front of an already-arrived CAN burst. The
         * bounded combined service below still guarantees RX/TX progress. */
        if (can_bus_rx_pending() < 16U)
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
            /* Service queued RX as well as TX between CAN batches. A TX-only
             * pass would strand the asynchronous handoff and overflow it. */
            /* A persistent CAN backlog must not strand owned network RX
             * packets. Service each bounded handoff batch before copying the
             * next one, so dispatch can release its memory. UART, maintenance
             * and the scheduler still run under the outer wall-time bound. */
            SERVICE(2, (void)process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS));
            if (received == 0U || can_bus_rx_pending() == 0U) break;
        } while ((uint32_t)(HAL_GetTick() - can_started) < TELEMETRY_CAN_SERVICE_BUDGET_MS);
        const uint32_t now = HAL_GetTick();
        if ((int32_t)(now - maintenance_due) >= 0) {
            maintenance_due = now + TELEMETRY_MAINTENANCE_PERIOD_MS;
            const uint8_t can_was_drained = can_bus_rx_pending() < 16U;
            SERVICE(3, (void)telemetry_poll_discovery());
            SERVICE(4, (void)telemetry_poll_timesync());
            /* Building a startup schema can exceed the maintenance budget.
             * IRQs continue filling CAN during that call. Catch a new burst
             * before stacking another expensive network operation on it.
             * A permanently busy ring keeps the original per-cycle bound. */
            if (can_was_drained && can_bus_rx_pending() >= 16U) {
                const uint32_t catchup_started = HAL_GetTick();
                do {
                    uint32_t received;
                    SERVICE(1, received = can_bus_process_rx_for(
                        TELEMETRY_CAN_FRAMES_PER_SLICE, TELEMETRY_CAN_SLICE_BUDGET_MS));
                    SERVICE(0, telemetry_uart_process());
                    SERVICE(2, (void)process_all_queues_timeout(TELEMETRY_QUEUE_SERVICE_BUDGET_MS));
                    if (received == 0U || can_bus_rx_pending() == 0U) break;
                } while ((uint32_t)(HAL_GetTick() - catchup_started) < TELEMETRY_CAN_SERVICE_BUDGET_MS);
            }
        }
        SERVICE(5, ota_stream_poll());
        /* The do/while above always services RX/TX, including an empty CAN
         * ring. Do not repeat discovery/time-sync maintenance here. OTA TX
         * drains at the start of the next loop before new CAN admission. */
#ifdef TELEMETRY_BOARD_LINK_UART
        board_link_uart_process();
#endif
        telemetry_hil_capture_requested_snapshot();
        sample_telemetry_stack();
        SERVICE(6, telemetry_wait_for_activity());
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

    UINT status = tx_semaphore_create(&telemetry_rx_activity, "CAN RX activity", 0U);
    if (status != TX_SUCCESS) return status;
    status = tx_thread_create(&telemetry_thread,
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
