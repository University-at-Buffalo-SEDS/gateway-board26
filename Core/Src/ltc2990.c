#include "ltc2990.h"
#include "main.h"
#include "sedsprintf.h"
#include "telemetry.h"

#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <stdio.h>
#include <inttypes.h>

// Global LTC2990 handle definition for telemetry
LTC2990_Handle_t ltc2990_handle;


static inline void sleep_ms(uint32_t ms)
{
    ULONG ticks = (ms * TX_TIMER_TICKS_PER_SECOND + 999u) / 1000u;

    if (tx_thread_identify() != TX_NULL) {
        tx_thread_sleep(ticks);
    } else {
        HAL_Delay(ms);
    }
}


static inline void i2c_lock(LTC2990_Handle_t *h)
{
    if (h->i2c_mutex && tx_thread_identify() != TX_NULL) {
        tx_mutex_get(h->i2c_mutex, TX_WAIT_FOREVER);
    }
}

static inline void i2c_unlock(LTC2990_Handle_t *h)
{
    if (h->i2c_mutex && tx_thread_identify() != TX_NULL) {
        tx_mutex_put(h->i2c_mutex);
    }
}


static inline uint8_t status_bit_from_msb(uint8_t msb_reg)
{
    switch (msb_reg) {
        case V1_MSB_REG: return 2;
        case V2_MSB_REG: return 3;
        case V3_MSB_REG: return 4;
        case V4_MSB_REG: return 5;
        default:         return 0xFF;
    }
}


int LTC2990_Init(LTC2990_Handle_t *h, I2C_HandleTypeDef *hi2c, uint8_t addr7, LTC2990_ROLE role)
{
    h->hi2c        = hi2c;
    h->i2c_address = addr7;
    h->role        = role;
    h->i2c_mutex   = NULL;

    for (int i = 0; i < 4; i++) {
        h->last_voltages[i] = NAN;
    }

    uint8_t control =
        (role == VOLTAGE) ?
        (CTRL_ALL | V1_V2_V3_V4) :
        (CTRL_ALL | MODE_DUAL_DIFF);

    uint8_t clear_mask = TEMP_MEAS_MODE_MASK | VOLTAGE_MODE_MASK;

    if (LTC2990_Set_Mode(h, control, clear_mask) != 0) {
        return 1;
    }

    if (role == VOLTAGE) {
        if (LTC2990_Enable_All_Voltages(h) != 0) {
            return 1;
        }
    }

    sleep_ms(100);
    LTC2990_Step(h);
    return 0;
}

void LTC2990_Step(LTC2990_Handle_t *h)
{
    LTC2990_Trigger_Conversion(h);
    sleep_ms(10);

    if (h->role == VOLTAGE) {
        const uint8_t regs[4] = {
            V1_MSB_REG, V2_MSB_REG, V3_MSB_REG, V4_MSB_REG
        };

        for (int i = 0; i < 4; i++) {
            uint16_t raw15;
            int8_t valid;

            if (LTC2990_ADC_Read_New_Data(h, regs[i], &raw15, &valid) == 0 && valid) {
                uint16_t code14 = raw15 & 0x3FFF;
                h->last_voltages[i] =
                    LTC2990_Code_To_Single_Ended_Voltage(h, code14);
            } else {
                h->last_voltages[i] = NAN;
            }
        }
    } else {
        uint16_t raw15_v1, raw15_v3;
        int8_t valid_v1, valid_v3;

        float current_v1 = NAN;
        float current_v3 = NAN;

        if (LTC2990_ADC_Read_New_Data(h, V1_MSB_REG, &raw15_v1, &valid_v1) == 0 && valid_v1) {
            current_v1 = LTC2990_Code15_To_CurrentA(raw15_v1);
        }

        if (LTC2990_ADC_Read_New_Data(h, V3_MSB_REG, &raw15_v3, &valid_v3) == 0 && valid_v3) {
            current_v3 = LTC2990_Code15_To_CurrentA(raw15_v3);
        }

        h->last_voltages[0] = current_v1;
        h->last_voltages[1] = current_v3;

        h->last_voltages[2] = NAN;
        h->last_voltages[3] = NAN;

    }
}


void LTC2990_Get_Voltage(LTC2990_Handle_t *h, float *voltages)
{
    for (int i = 0; i < 4; i++) {
        voltages[i] = h->last_voltages[i];
    }
}


int8_t LTC2990_Enable_All_Voltages(LTC2990_Handle_t *h)
{
    return LTC2990_Set_Mode(h, ENABLE_ALL, TEMP_MEAS_MODE_MASK);
}

int8_t LTC2990_Set_Mode(LTC2990_Handle_t *h,
                        uint8_t bits_to_set,
                        uint8_t bits_to_clear)
{
    uint8_t reg;
    if (LTC2990_Read_Register(h, CONTROL_REG, &reg) != 0) {
        return 1;
    }

    reg &= ~bits_to_clear;
    reg |= bits_to_set;

    return LTC2990_Write_Register(h, CONTROL_REG, reg);
}

int8_t LTC2990_Trigger_Conversion(LTC2990_Handle_t *h)
{
    return LTC2990_Write_Register(h, TRIGGER_REG, 0x00);
}


uint8_t LTC2990_ADC_Read_New_Data(LTC2990_Handle_t *h, uint8_t msb_reg, uint16_t *raw15, int8_t *data_valid)
{
    uint16_t timeout = TIMEOUT;
    uint8_t status;
    uint8_t bit = status_bit_from_msb(msb_reg);

    if (bit == 0xFF) return 1;

    while (--timeout) {
        if (LTC2990_Read_Register(h, STATUS_REG, &status) != 0) return 1;
        if ((status >> bit) & 0x01) break;
        sleep_ms(1);
    }

    if (!timeout) return 1;

    uint8_t msb, lsb;
    if (LTC2990_Read_Register(h, msb_reg, &msb) != 0) return 1;
    if (LTC2990_Read_Register(h, msb_reg + 1, &lsb) != 0) return 1;

    uint16_t code = ((uint16_t)msb << 8) | lsb;

    *data_valid = (code >> 15) & 0x01;
    *raw15      = code & 0x7FFF;

    return (*data_valid) ? 0 : 1;
}


float LTC2990_Code_To_Single_Ended_Voltage(LTC2990_Handle_t *handle, uint16_t code14)
{
    (void)handle;
    return (float)(code14 & 0x3FFF) * SINGLE_ENDED_LSB;
}

float LTC2990_Code15_To_CurrentA(uint16_t raw15)
{
    const float a_per_count = 19.42e-6f / (RSENSE_OHM * CURRENT_DIVIDER_RATIO);
    const uint16_t magnitude = raw15 & 0x3FFFU;

    if ((raw15 & 0x4000U) != 0U) {
        return -((float)magnitude + 1.0f) * a_per_count;
    }

    return (float)magnitude * a_per_count;
}


int8_t LTC2990_Read_Register(LTC2990_Handle_t *h, uint8_t reg, uint8_t *data)
{
    i2c_lock(h);

    HAL_StatusTypeDef st =
        HAL_I2C_Mem_Read(h->hi2c, h->i2c_address << 1, reg, I2C_MEMADD_SIZE_8BIT, data, 1, TIMEOUT);

    i2c_unlock(h);
    return (st == HAL_OK) ? 0 : 1;
}

int8_t LTC2990_Write_Register(LTC2990_Handle_t *h, uint8_t reg, uint8_t data)
{
    i2c_lock(h);

    HAL_StatusTypeDef st =
        HAL_I2C_Mem_Write(h->hi2c, h->i2c_address << 1, reg, I2C_MEMADD_SIZE_8BIT, &data, 1, TIMEOUT);

    i2c_unlock(h);
    return (st == HAL_OK) ? 0 : 1;
}

void telemetry_ltc2990_update_voltage(LTC2990_Handle_t *ltc2990_handle) {
    float voltages[4] = {0, 0, 0, 0};
    LTC2990_Step(ltc2990_handle);
    LTC2990_Get_Voltage(ltc2990_handle, voltages);
    float voltage = (voltages[0] * VBATT_DIVIDER_GAIN) - VBATT_OFFSET_V;


    SedsResult res = log_telemetry_asynchronous(SEDS_DT_BATTERY_VOLTAGE, &voltage, 1, sizeof(float));
    if (res != SEDS_OK) {
        Error_Handler();
    }
}


void telemetry_ltc2990_update_current(LTC2990_Handle_t *ltc2990_handle) {
    float current[4] = {0, 0, 0, 0};
    LTC2990_Step(ltc2990_handle);
    LTC2990_Get_Voltage(ltc2990_handle, current);
    float current_value = current[CURRENT_TELEMETRY_CHANNEL_INDEX] * CURRENT_DRAW_POLARITY;

    if (isnan(current_value)) {
        return;
    }


    SedsResult res = log_telemetry_asynchronous(SEDS_DT_BATTERY_CURRENT, &current_value, 1, sizeof(float));
    if (res != SEDS_OK) {
        Error_Handler();
    }
}
