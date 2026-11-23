// data_acq.c
#include "GB-Threads.h"
#include "ltc2990.h"
#include "main.h"
#ifdef TELEMETRY_ENABLED
#include "telemetry.h"
#endif
#include "board_watchdog.h"
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

/* Absent or failing LTC2990: retry with exponential backoff so a missing
 * device does not clear the bus (and disturb the working device) every
 * report period. The first retry is one period after the failure, then
 * the delay doubles up to POWER_I2C_RETRY_MAX_MS. Any successful retry
 * returns to the normal period. */
#define POWER_I2C_RETRY_MIN_MS 1000U
#define POWER_I2C_RETRY_MAX_MS 32000U
#define POWER_LTC2990_VOLTAGE 1U
#define POWER_LTC2990_CURRENT 2U
#define POWER_LTC2990_ALL (POWER_LTC2990_VOLTAGE | POWER_LTC2990_CURRENT)

typedef struct {
    uint32_t next_ms;
    uint32_t delay_ms;
    uint8_t failing;
} power_retry_t;

/* Health (BOARD_HEALTH words 16..21, "LTC1"): devices answering, current
 * retry delay (0 healthy), retries attempted. */
volatile uint32_t g_power_ltc2990_present;
volatile uint32_t g_power_i2c_retry_delay_ms;
volatile uint32_t g_power_i2c_retries;

static int power_retry_due(const power_retry_t *r, uint32_t now_ms)
{
    return !r->failing || (int32_t)(now_ms - r->next_ms) >= 0;
}

static void power_retry_result(power_retry_t *r, uint32_t now_ms, int healthy)
{
    if (healthy) {
        r->failing = 0U;
        r->delay_ms = 0U;
        return;
    }
    if (!r->failing || r->delay_ms < POWER_I2C_RETRY_MIN_MS) {
        r->failing = 1U;
        r->delay_ms = POWER_I2C_RETRY_MIN_MS;
    } else {
        r->delay_ms = (r->delay_ms >= POWER_I2C_RETRY_MAX_MS / 2U)
            ? POWER_I2C_RETRY_MAX_MS : r->delay_ms * 2U;
    }
    r->next_ms = now_ms + r->delay_ms;
}

static uint32_t data_acq_ready_mask(void)
{
    return (ltc2990_voltage_ready ? POWER_LTC2990_VOLTAGE : 0U) |
           (ltc2990_current_ready ? POWER_LTC2990_CURRENT : 0U);
}

/* Initializes the devices in `which`; logs a failure only when `log`. */
static void data_acq_ltc2990_init(uint32_t which, int log)
{
    if ((which & POWER_LTC2990_VOLTAGE) != 0U) {
        ltc2990_voltage_ready =
            (LTC2990_Init(&ltc2990_voltage_handle, &hi2c2, LTC2990_I2C_ADDRESS_VOLTAGE, VOLTAGE) == 0)
                ? 1U
                : 0U;
        if (ltc2990_voltage_ready == 0U && log) {
#ifdef TELEMETRY_ENABLED
            (void)log_error_asynchronous("LTC2990 voltage init failed");
#endif
        }
    }

    if ((which & POWER_LTC2990_CURRENT) != 0U) {
        ltc2990_current_ready =
            (LTC2990_Init(&ltc2990_current_handle, &hi2c2, LTC2990_I2C_ADDRESS_CURRENT, CURRENT) == 0)
                ? 1U
                : 0U;
        if (ltc2990_current_ready == 0U && log) {
#ifdef TELEMETRY_ENABLED
            (void)log_error_asynchronous("LTC2990 current init failed");
#endif
        }
    }
    g_power_ltc2990_present = data_acq_ready_mask();
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

static int power_i2c_lines_released(void)
{
    return HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_8) == GPIO_PIN_SET &&
           HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_9) == GPIO_PIN_SET;
}

/* One supervision pass. A held bus or a bus-level HAL error gets the full
 * bus clear and re-init of both devices. A device that only NACKs (absent)
 * is probed alone, so the working device is not reset. */
static void data_acq_supervise(power_retry_t *retry, uint32_t now_ms)
{
    const uint32_t error = HAL_I2C_GetError(&hi2c2);
    const uint32_t missing = POWER_LTC2990_ALL & ~data_acq_ready_mask();
    if (error == HAL_I2C_ERROR_NONE && missing == 0U) {
        power_retry_result(retry, now_ms, 1);
        g_power_i2c_retry_delay_ms = 0U;
        return;
    }
    if (!power_retry_due(retry, now_ms)) return;
    const int log = !retry->failing;
    ++g_power_i2c_retries;
    if (!power_i2c_lines_released() || (error & ~(uint32_t)HAL_I2C_ERROR_AF) != 0U) {
        if (power_i2c_recover()) data_acq_ltc2990_init(POWER_LTC2990_ALL, log);
    } else {
        data_acq_ltc2990_init(missing, log);
    }
    power_retry_result(retry, now_ms,
                       data_acq_ready_mask() == POWER_LTC2990_ALL &&
                       HAL_I2C_GetError(&hi2c2) == HAL_I2C_ERROR_NONE);
    g_power_i2c_retry_delay_ms = retry->failing ? retry->delay_ms : 0U;
}

static uint32_t data_acq_now_ms(void)
{
    return (uint32_t)(((uint64_t)tx_time_get() * 1000ULL) / TX_TIMER_TICKS_PER_SECOND);
}

void data_acq_thread_entry(ULONG initial_input)
{
    (void)initial_input;
    power_retry_t retry = {0};

    if (!power_i2c_lines_released()) {
        (void)power_i2c_recover();
    }
    data_acq_ltc2990_init(POWER_LTC2990_ALL, 1);
    power_retry_result(&retry, data_acq_now_ms(),
                       data_acq_ready_mask() == POWER_LTC2990_ALL);
    g_power_i2c_retry_delay_ms = retry.failing ? retry.delay_ms : 0U;

    for (;;) {
        data_acq_supervise(&retry, data_acq_now_ms());
        data_acq_report_power();
        board_watchdog_progress(BOARD_WATCHDOG_ACQUISITION);
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
