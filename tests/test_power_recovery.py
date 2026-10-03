import pathlib
import subprocess
import tempfile
import unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
class PowerRecoveryTests(unittest.TestCase):
    def test_bus_clear_is_bounded_and_restores_peripheral(self):
        source = (ROOT/'Core/Src/data_acq.c').read_text()
        recovery = source[source.index('static int power_i2c_recover('):source.index('static void data_acq_ltc2990_init(')]
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
