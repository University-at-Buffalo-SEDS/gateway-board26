import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class PowerRecoveryTests(unittest.TestCase):
    def test_bus_clear_is_bounded_and_restores_peripheral(self):
        source = (ROOT/'Core/Src/data_acq.c').read_text()
        recovery = source[source.index('static int power_i2c_recover('):source.index('/* Absent or failing LTC2990')]
        code = r'''
#include <assert.h>
#include <stdint.h>
typedef struct { unsigned Pin, Mode, Pull, Speed; } GPIO_InitTypeDef;
#define GPIO_PIN_8 256
#define GPIO_PIN_9 512
#define GPIO_PIN_SET 1
#define GPIO_PIN_RESET 0
#define GPIO_MODE_OUTPUT_OD 3
#define GPIO_NOPULL 0
#define GPIO_SPEED_FREQ_LOW 0
#define GPIOA 0
#define HAL_OK 0
static int hi2c2, pulses, released_after, scl_stuck, init_fail, initialized, sleeps;
static int scl=1, sda=1;
static uint32_t g_power_i2c_recoveries, g_power_i2c_recovery_failures;
static int HAL_I2C_DeInit(int *p) { assert(p==&hi2c2); return 0; }
static int HAL_I2C_Init(int *p) { assert(p==&hi2c2); initialized++; return init_fail; }
static void HAL_GPIO_Init(int p, GPIO_InitTypeDef *g) {
    (void)p; assert(g->Mode==GPIO_MODE_OUTPUT_OD); assert(g->Pull==GPIO_NOPULL);
    assert(g->Pin==(GPIO_PIN_8|GPIO_PIN_9));
}
static void HAL_GPIO_WritePin(int p, int pins, int value) {
    (void)p;
    if (pins & GPIO_PIN_9) { if (!scl && value && sda) pulses++; scl=value; }
    if (pins & GPIO_PIN_8) sda=value;
}
static int HAL_GPIO_ReadPin(int p, int pin) {
    (void)p;
    if (pin==GPIO_PIN_9) return scl && !scl_stuck;
    return sda && pulses>=released_after;
}
static void tx_thread_sleep(int n) { assert(n==1); sleeps++; }
''' + recovery + r'''
int main(void) {
    released_after=4;
    assert(power_i2c_recover()==1); assert(pulses==4); assert(initialized==1);
    assert(g_power_i2c_recoveries==1);
    pulses=0; sleeps=0; released_after=100;
    assert(power_i2c_recover()==0); assert(pulses==9); assert(sleeps==21);
    assert(initialized==2 && g_power_i2c_recovery_failures==1);
    pulses=0; scl_stuck=1;
    assert(power_i2c_recover()==0); assert(pulses==1); assert(initialized==3);
    scl_stuck=0; released_after=0; init_fail=1;
    assert(power_i2c_recover()==0); assert(initialized==4);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            out=str(pathlib.Path(tmp)/'power-recovery')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-x','c','-','-o',out],input=code,text=True,check=True)
            subprocess.run([out],check=True)

    def test_absent_device_backs_off_and_recovers(self):
        source = (ROOT/'Core/Src/data_acq.c').read_text()
        block = source[source.index('/* Absent or failing LTC2990'):source.index('static uint32_t data_acq_now_ms(')]
        code = r"""
#include <assert.h>
#include <stdint.h>
typedef struct { int unused; } LTC2990_Handle_t;
typedef enum { VOLTAGE, CURRENT } LTC2990_ROLE;
#define LTC2990_I2C_ADDRESS_VOLTAGE 0x4C
#define LTC2990_I2C_ADDRESS_CURRENT 0x4E
#define HAL_I2C_ERROR_NONE 0U
#define HAL_I2C_ERROR_AF 4U
#define HAL_I2C_ERROR_BERR 1U
#define GPIOA 0
#define GPIO_PIN_8 256
#define GPIO_PIN_9 512
#define GPIO_PIN_SET 1
#define TELEMETRY_ENABLED
static int hi2c2;
static LTC2990_Handle_t ltc2990_voltage_handle, ltc2990_current_handle;
static uint8_t ltc2990_voltage_ready, ltc2990_current_ready;
static uint32_t i2c_error, bus_clears, logs, inits[2], volatile_present=1, current_present=0;
static int lines_low;
static uint32_t HAL_I2C_GetError(int *h) { assert(h==&hi2c2); return i2c_error; }
static int HAL_GPIO_ReadPin(int port, int pin) { (void)port; (void)pin; return !lines_low; }
static int power_i2c_recover(void) { bus_clears++; i2c_error = 0; return 1; }
static int LTC2990_Init(LTC2990_Handle_t *h, int *bus, uint8_t addr, LTC2990_ROLE role) {
    assert(bus==&hi2c2); (void)addr;
    inits[role]++;
    const int present = role == VOLTAGE ? volatile_present : current_present;
    assert(h == (role == VOLTAGE ? &ltc2990_voltage_handle : &ltc2990_current_handle));
    if (!present) i2c_error = HAL_I2C_ERROR_AF; /* absent: address NACK */
    return present ? 0 : -1;
}
static int log_error_asynchronous(const char *m) { (void)m; logs++; return 0; }
static void telemetry_ltc2990_update_voltage(LTC2990_Handle_t *h) { (void)h; i2c_error = 0; }
static void telemetry_ltc2990_update_current(LTC2990_Handle_t *h) { (void)h; i2c_error = 0; }
""" + block + r"""
/* One report pass per second, like data_acq_thread_entry. */
static power_retry_t retry;
static uint32_t now;
static void pass(void) { data_acq_supervise(&retry, now); data_acq_report_power(); now += 1000U; }
int main(void) {
    /* Boot: the current monitor is absent. */
    data_acq_ltc2990_init(POWER_LTC2990_ALL, 1);
    power_retry_result(&retry, now, data_acq_ready_mask() == POWER_LTC2990_ALL);
    assert(g_power_ltc2990_present == POWER_LTC2990_VOLTAGE && logs == 1);
    now += 1000U;
    /* Ten minutes: retries back off 1,2,4,8,16,32,32... s; never a bus clear,
     * and the working voltage monitor is never re-initialised. */
    uint32_t probes_at[64], probes = 0, last = inits[CURRENT];
    while (now < 600000U) {
        pass();
        if (inits[CURRENT] != last) { assert(probes < 64); probes_at[probes++] = now - 1000U; last = inits[CURRENT]; }
    }
    assert(bus_clears == 0 && inits[VOLTAGE] == 1 && logs == 1);
    assert(probes_at[0] == 1000U && probes_at[1] == 3000U && probes_at[2] == 7000U);
    for (uint32_t i = 1; i < probes; i++) {
        const uint32_t gap = probes_at[i] - probes_at[i - 1];
        assert(gap <= POWER_I2C_RETRY_MAX_MS && gap >= 1000U);
        if (i >= 6) assert(gap == POWER_I2C_RETRY_MAX_MS);
    }
    assert(probes < 30); /* was 600 bus clears in the same ten minutes */
    assert(g_power_i2c_retry_delay_ms == POWER_I2C_RETRY_MAX_MS);
    assert(g_power_ltc2990_present == POWER_LTC2990_VOLTAGE);
    /* The device appears: picked up within one maximum delay, then healthy. */
    current_present = 1;
    const uint32_t appeared = now;
    while (!(g_power_ltc2990_present & POWER_LTC2990_CURRENT)) { pass(); assert(now - appeared <= POWER_I2C_RETRY_MAX_MS + 1000U); }
    pass();
    assert(g_power_ltc2990_present == POWER_LTC2990_ALL && g_power_i2c_retry_delay_ms == 0U && !retry.failing);
    const uint32_t probes_healthy = inits[CURRENT];
    for (int i = 0; i < 100; i++) pass();
    assert(inits[CURRENT] == probes_healthy && bus_clears == 0);
    /* A held bus still gets the full bus clear, then re-init of both. */
    lines_low = 1; i2c_error = HAL_I2C_ERROR_BERR;
    pass(); assert(bus_clears == 1 && inits[VOLTAGE] == 2); lines_low = 0;
    pass(); assert(!retry.failing && g_power_i2c_retry_delay_ms == 0U);
    /* Backoff restarts at the minimum after a recovery. */
    current_present = 0; ltc2990_current_ready = 0; g_power_ltc2990_present = POWER_LTC2990_VOLTAGE;
    const uint32_t before = inits[CURRENT];
    pass(); assert(inits[CURRENT] == before + 1 && g_power_i2c_retry_delay_ms == POWER_I2C_RETRY_MIN_MS);
    pass(); assert(inits[CURRENT] == before + 2 && g_power_i2c_retry_delay_ms == 2000U);
    /* Delay saturates and survives 32-bit millisecond wrap. */
    now = UINT32_MAX - 5000U;
    retry = (power_retry_t){0}; power_retry_result(&retry, now, 0); now += 1000U;
    const uint32_t wrap_probes = inits[CURRENT];
    for (int i = 0; i < 200; i++) pass();
    assert(g_power_i2c_retry_delay_ms == POWER_I2C_RETRY_MAX_MS);
    assert(inits[CURRENT] - wrap_probes >= 9 && inits[CURRENT] - wrap_probes <= 12);
    return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            out=str(pathlib.Path(tmp)/'power-backoff')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c','-','-o',out],input=code,text=True,check=True)
            subprocess.run([out],check=True)
