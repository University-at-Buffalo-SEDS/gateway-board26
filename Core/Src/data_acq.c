// data_acq.c
#include "GB-Threads.h"
#include "ltc2990.h"
#include "main.h"
#include "telemetry.h"
#include "tx_api.h"
#include <stdint.h>

extern I2C_HandleTypeDef hi2c2;

TX_THREAD data_acq_thread;

#define DATA_ACQ_THREAD_STACK_SIZE (8U * 1024U)
#define DATA_ACQ_REPORT_PERIOD_TICKS ((ULONG)TX_TIMER_TICKS_PER_SECOND)

static LTC2990_Handle_t ltc2990_voltage_handle;
static LTC2990_Handle_t ltc2990_current_handle;
static uint8_t ltc2990_voltage_ready;
static uint8_t ltc2990_current_ready;

static void data_acq_ltc2990_init(void)
{
    ltc2990_voltage_ready =
        (LTC2990_Init(&ltc2990_voltage_handle, &hi2c2, LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE) == 0)
            ? 1U
            : 0U;
    if (ltc2990_voltage_ready == 0U) {
        (void)log_error_asynchronous("LTC2990 voltage init failed");
    }

    ltc2990_current_ready =
        (LTC2990_Init(&ltc2990_current_handle, &hi2c2, LTC2990_I2C_ADDRESS_CURRENT, CURRENT) == 0)
            ? 1U
            : 0U;
    if (ltc2990_current_ready == 0U) {
        (void)log_error_asynchronous("LTC2990 current init failed");
    }
}

static void data_acq_report_power(void)
{
    if (ltc2990_voltage_ready != 0U) {
        telemetry_ltc2990_update_voltage(&ltc2990_voltage_handle);
    }

    if (ltc2990_current_ready != 0U) {
        telemetry_ltc2990_update_current(&ltc2990_current_handle);
    }
}

void data_acq_thread_entry(ULONG initial_input)
{
    (void)initial_input;

    data_acq_ltc2990_init();

    for (;;) {
        data_acq_report_power();
        // HAL_GPIO_TogglePin(GREEN_LED_GPIO_Port, GREEN_LED_Pin);
        tx_thread_sleep(DATA_ACQ_REPORT_PERIOD_TICKS);
    }
}

UINT create_data_acq_thread(TX_BYTE_POOL *byte_pool)
{
    CHAR *pointer;

    if (tx_byte_allocate(byte_pool, (VOID **)&pointer,
                         DATA_ACQ_THREAD_STACK_SIZE, TX_NO_WAIT) != TX_SUCCESS) {
        return TX_POOL_ERROR;
    }

    return tx_thread_create(&data_acq_thread,
                            "Data Acquisition Thread",
                            data_acq_thread_entry,
                            0,
                            pointer,
                            DATA_ACQ_THREAD_STACK_SIZE,
                            6,
                            6,
                            TX_NO_TIME_SLICE,
                            TX_AUTO_START);
}
