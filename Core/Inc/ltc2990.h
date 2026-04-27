#ifndef LTC2990_H
#define LTC2990_H

#include "stm32g4xx_hal.h"
#include <math.h>
#include <stdint.h>

#include "tx_api.h"

#define STATUS_REG          (0x00)
#define CONTROL_REG         (0x01)
#define TRIGGER_REG         (0x02)

#define V1_MSB_REG          (0x06)
#define V1_LSB_REG          (0x07)
#define V2_MSB_REG          (0x08)
#define V2_LSB_REG          (0x09)
#define V3_MSB_REG          (0x0A)
#define V3_LSB_REG          (0x0B)
#define V4_MSB_REG          (0x0C)
#define V4_LSB_REG          (0x0D)

#define MODE_V1mV2_TR2      (0x01)
#define MODE_DUAL_DIFF     (0x06)

#define VOLTAGE_MODE_MASK   (0x07)
#define TEMP_MEAS_MODE_MASK (0x18)

#define V1_V2_V3_V4         (0x07)
#define ENABLE_ALL          (0x18)

#define CTRL_ALL            (3u << 3)   // b[4:3] = 11
#define CTRL_V1_ONLY        (1u << 3)   // b[4:3] = 01 

#define SINGLE_ENDED_LSB    (5.0f / 16384.0f)  // 5V  2^14
#define VBATT_DIVIDER_TOP_OHM       (71500.0f)
#define VBATT_DIVIDER_BOTTOM_OHM    (10000.0f)
#define VBATT_DIVIDER_GAIN          ((VBATT_DIVIDER_TOP_OHM + VBATT_DIVIDER_BOTTOM_OHM) / \
                                     VBATT_DIVIDER_BOTTOM_OHM)
#define VBATT_OFFSET_V              (0.07f)
#define RSENSE_OHM          (0.02f)
#define CURRENT_DIVIDER_TOP_OHM    (71500.0f)
#define CURRENT_DIVIDER_BOTTOM_OHM (10000.0f)
#define CURRENT_DIVIDER_RATIO      (CURRENT_DIVIDER_BOTTOM_OHM / \
                                     (CURRENT_DIVIDER_TOP_OHM + CURRENT_DIVIDER_BOTTOM_OHM))
#define CURRENT_TELEMETRY_CHANNEL_INDEX (0U)
#define CURRENT_DRAW_POLARITY      (-1.0f)

#define TIMEOUT             1000


#define LTC2990_I2C_ADDRESS_VOLTAGE (0x4C)
#define LTC2990_I2C_ADDRESS_CURRENT (0x4D)


typedef enum {
    VOLTAGE,
    CURRENT
} LTC2990_ROLE;


typedef struct {
    I2C_HandleTypeDef *hi2c;
    uint8_t            i2c_address;
    LTC2990_ROLE       role;

    float              last_voltages[4];
    TX_MUTEX          *i2c_mutex;
} LTC2990_Handle_t;


int  LTC2990_Init(LTC2990_Handle_t *handle, I2C_HandleTypeDef *hi2c, uint8_t addr7, LTC2990_ROLE role);

void LTC2990_Step(LTC2990_Handle_t *handle);
void LTC2990_Get_Voltage(LTC2990_Handle_t *handle, float *voltages);

int8_t LTC2990_Enable_All_Voltages(LTC2990_Handle_t *handle);
int8_t LTC2990_Set_Mode(LTC2990_Handle_t *handle, uint8_t bits_to_set, uint8_t bits_to_clear);

int8_t LTC2990_Trigger_Conversion(LTC2990_Handle_t *handle);

uint8_t LTC2990_ADC_Read_New_Data(LTC2990_Handle_t *handle, uint8_t msb_register_address, uint16_t *raw15, int8_t *data_valid);


float LTC2990_Code_To_Single_Ended_Voltage(LTC2990_Handle_t *handle, uint16_t code14);

float LTC2990_Code15_To_CurrentA(uint16_t raw15);


int8_t LTC2990_Read_Register(LTC2990_Handle_t *handle, uint8_t reg_address, uint8_t *data);

int8_t LTC2990_Write_Register(LTC2990_Handle_t *handle, uint8_t reg_address, uint8_t data);

extern void CDC_Transmit_Print(const char *format, ...);

void telemetry_ltc2990_update_voltage(LTC2990_Handle_t *ltc2990_handle);

void telemetry_ltc2990_update_current(LTC2990_Handle_t *ltc2990_handle);
#endif 
