#ifndef GATEWAY_HANDLER_H
#define GATEWAY_HANDLER_H

#include "stm32g4xx_hal.h"
#include "cmsis_os.h"

/* Enum for Board Addressing */
typedef enum {
    TARGET_VALVE    = 0x100, // DSUB Umbilical
    TARGET_ACTUATOR = 0x200, // Ribbon Cable
    TARGET_DAQ      = 0x300, // Ribbon Cable
    TARGET_UNKNOWN  = 0x000
} BoardTarget_t;

typedef struct {
    BoardTarget_t target;
    uint8_t command;
    uint8_t data[6];
} GatewayPacket_t;

/* Function Prototypes returning HAL_StatusTypeDef */
HAL_StatusTypeDef Gateway_Init(void);
HAL_StatusTypeDef Handle_Actuator_GPIO(uint8_t command);
void StartCommandTask(void *argument);
void StartTelemetryTask(void *argument);

extern osMessageQueueId_t cmdQueueHandle;
extern osMessageQueueId_t telQueueHandle;

#endif