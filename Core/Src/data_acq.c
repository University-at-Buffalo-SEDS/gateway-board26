// data_acq.c
#include "GB-Threads.h"
#include "ltc2990.h"
#include "main.h"
#include "telemetry.h"
#include "tx_api.h"
#include <stdint.h>

extern I2C_HandleTypeDef hi2c2;

TX_THREAD data_acq_thread;

#define DATA_ACQ_THREAD_STACK_SIZE (4U * 1024U)
#define DATA_ACQ_REPORT_PERIOD_TICKS ((ULONG)TX_TIMER_TICKS_PER_SECOND)

static LTC2990_Handle_t ltc2990_voltage_handle;
static LTC2990_Handle_t ltc2990_current_handle;
static uint8_t ltc2990_voltage_ready;
static uint8_t ltc2990_current_ready;

volatile uint32_t g_power_i2c_recoveries;
volatile uint32_t g_power_i2c_recovery_failures;

/* Sole I2C2 worker. Release a target left mid-byte by an MCU reset.
 * NXP UM10204 bus clear: at most nine open-drain SCL pulses, then STOP. */
static int power_i2c_recover(void)
{
    (void)HAL_I2C_DeInit(&hi2c2);
    GPIO_InitTypeDef gpio = {0};
    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9;
    gpio.Mode = GPIO_MODE_OUTPUT_OD;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_WritePin(GPIOA, gpio.Pin, GPIO_PIN_SET);
    HAL_GPIO_Init(GPIOA, &gpio);
    for (unsigned pulse = 0; pulse < 9; ++pulse) {
        if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_SET) break;
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_RESET);
        tx_thread_sleep(1);
        HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET);
        tx_thread_sleep(1);
        if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) != GPIO_PIN_SET) break;
    }
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    tx_thread_sleep(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_9, GPIO_PIN_SET);
    tx_thread_sleep(1);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_SET);
    tx_thread_sleep(1);
    const int released = HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_SET &&
                         HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) == GPIO_PIN_SET;
    const int initialized = HAL_I2C_Init(&hi2c2) == HAL_OK;
    if (released && initialized) { g_power_i2c_recoveries++; return 1; }
    g_power_i2c_recovery_failures++;
    return 0;
}

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

    if (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) != GPIO_PIN_SET ||
        HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) != GPIO_PIN_SET) {
        (void)power_i2c_recover();
    }
    data_acq_ltc2990_init();

    for (;;) {
        if (HAL_I2C_GetError(&hi2c2) != HAL_I2C_ERROR_NONE ||
            !ltc2990_voltage_ready || !ltc2990_current_ready) {
            if (power_i2c_recover()) data_acq_ltc2990_init();
        }
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
