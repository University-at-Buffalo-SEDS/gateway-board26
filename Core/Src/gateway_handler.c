#include "gateway_handler.h"
#include "fdcan.h"

HAL_StatusTypeDef Gateway_Init(void) {
    HAL_StatusTypeDef status;
    
    status = HAL_FDCAN_Start(&hfdcan1);
    if (status != HAL_OK) return status;

    status = HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    return status;
}

/* GPIO Logic based on Schematic */
HAL_StatusTypeDef Handle_Actuator_GPIO(uint8_t command) {
    // We assume HAL_OK initially; HAL_GPIO_WritePin doesn't return status, 
    // but in a custom driver you might check register states here.
    switch(command) {
        case 0x01: // Igniter (PB15)
            HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_SET);
            break;
        case 0x02: // N2O_Sig (PC6)
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
            break;
        case 0x03: // N2_Sig (PC12)
            HAL_GPIO_WritePin(GPIOC, GPIO_PIN_12, GPIO_PIN_SET);
            break;
        default:
            return HAL_ERROR;
    }
    return HAL_OK;
}

void StartCommandTask(void *argument) {
    GatewayPacket_t pkg;
    FDCAN_TxHeaderTypeDef TxHeader;

    for(;;) {
        if (osMessageQueueGet(cmdQueueHandle, &pkg, NULL, osWaitForever) == osOK) {
            //specifically bc actuator board uses GPIO input/output too
            if (pkg.target == TARGET_ACTUATOR) {
                if (Handle_Actuator_GPIO(pkg.command) != HAL_OK) {
                    // Logic for failed GPIO command
                }
            }

            TxHeader.Identifier = pkg.target;
            TxHeader.IdType = FDCAN_STANDARD_ID;
            TxHeader.DataLength = FDCAN_DLC_BYTES_8;
            
            if (HAL_FDCAN_AddMessageToTxFifoQ(&hfdcan1, &TxHeader, (uint8_t*)&pkg.command) != HAL_OK) {
                // Handle CAN Bus failure
            }
        }
    }
}

extern UART_HandleTypeDef huart2;

void StartTelemetryTask(void *argument) {
    for(;;) {
        GatewayPacket_t tel;
        if (osMessageQueueGet(telQueueHandle, &tel, NULL, 10) == osOK) {
            HAL_UART_Transmit(&huart2, (uint8_t*)&tel, sizeof(tel), 10);
        }

        if (HAL_GPIO_ReadPin(GPIOC, GPIO_PIN_11) == GPIO_PIN_SET) {
            GatewayPacket_t fault = {TARGET_ACTUATOR, 0xEE};
            HAL_UART_Transmit(&huart2, (uint8_t*)&fault, sizeof(fault), 10);
        }
        osDelay(100); 
    }
}
