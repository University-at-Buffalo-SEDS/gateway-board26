"""Fault injection against the actual foreground recovery function, not a copied implementation."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class CanRestartRecovery(unittest.TestCase):
 def test_fault_clear_restarts_and_notification_failure_retries_without_resetting_sdk(self):
  source=(ROOT/'Core/Src/can_bus.c').read_text();start=source.index('volatile uint32_t g_fdcan_tx_complete_events;');end=source.index('static can_bus_rx_frame_t g_rx_ring',start);sut=source[start:end]
  code=r'''
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include "can_transport_health.h"
typedef enum{HAL_OK,HAL_ERROR,HAL_BUSY}HAL_StatusTypeDef;
#define HAL_FDCAN_STATE_BUSY 2
#define HAL_FDCAN_STATE_READY 1
#define HAL_FDCAN_STATE_ERROR 3
#define FDCAN_TX_BUFFER0 1
#define FDCAN_TX_BUFFER1 2
#define FDCAN_TX_BUFFER2 4
#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE 1
#define FDCAN_IT_RX_FIFO1_NEW_MESSAGE 2
#define FDCAN_IT_RX_FIFO0_MESSAGE_LOST 4
#define FDCAN_IT_RX_FIFO1_MESSAGE_LOST 8
#define FDCAN_IT_TX_FIFO_EMPTY 16
#define FDCAN_IT_TX_COMPLETE 32
#define CAN_BUS_POLLING 0
typedef struct{uint32_t TXBRP,PSR,ECR;}Registers;
static Registers registers;
typedef struct{int State;Registers *Instance;}FDCAN_HandleTypeDef;
typedef struct{unsigned BusOff;}FDCAN_ProtocolStatusTypeDef;
static FDCAN_HandleTypeDef handle={HAL_FDCAN_STATE_BUSY,&registers},*g_hfdcan=&handle;
static unsigned bus_off,stop_timeout,fail_init,fail_filter,fail_notify,starts,stops,inits,resets,notifications;
static uint32_t now;
static unsigned g_fdcan_bus_off_count,g_fdcan_recovery_count;
static volatile uint8_t g_can_recovering;
static uint32_t HAL_GetTick(void){return now;}
static HAL_StatusTypeDef HAL_FDCAN_GetProtocolStatus(FDCAN_HandleTypeDef*h,FDCAN_ProtocolStatusTypeDef*p){(void)h;p->BusOff=bus_off;return HAL_OK;}
static HAL_StatusTypeDef HAL_FDCAN_AbortTxRequest(FDCAN_HandleTypeDef*h,unsigned n){assert(g_can_recovering);(void)h;(void)n;return HAL_OK;}
/* Checked-in ST HAL state contract: Stop timeout leaves ERROR. Start needs READY. */
static HAL_StatusTypeDef HAL_FDCAN_Stop(FDCAN_HandleTypeDef*h){assert(g_can_recovering);++stops;if(h->State!=HAL_FDCAN_STATE_BUSY)return HAL_ERROR;if(stop_timeout){stop_timeout=0;h->State=HAL_FDCAN_STATE_ERROR;return HAL_ERROR;}h->State=HAL_FDCAN_STATE_READY;return HAL_OK;}
static __attribute__((unused)) HAL_StatusTypeDef HAL_FDCAN_Init(FDCAN_HandleTypeDef*h){assert(g_can_recovering);++inits;if(fail_init)return HAL_ERROR;h->State=HAL_FDCAN_STATE_READY;return HAL_OK;}
static HAL_StatusTypeDef HAL_FDCAN_Start(FDCAN_HandleTypeDef*h){assert(g_can_recovering);++starts;if(h->State!=HAL_FDCAN_STATE_READY)return HAL_ERROR;h->State=HAL_FDCAN_STATE_BUSY;bus_off=0;return HAL_OK;}
static __attribute__((unused)) HAL_StatusTypeDef HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef*h,unsigned mask,unsigned buffers){assert(g_can_recovering);assert(h->State==HAL_FDCAN_STATE_BUSY);assert(mask==63&&buffers==7);++notifications;return fail_notify?HAL_ERROR:HAL_OK;}
static void can_tx_queue_reset(void){assert(g_can_recovering);++resets;}
'''+sut+r'''
static __attribute__((unused)) HAL_StatusTypeDef can_bus_configure_filters(FDCAN_HandleTypeDef*h){assert(g_can_recovering);assert(h->State==HAL_FDCAN_STATE_READY);return fail_filter?HAL_ERROR:HAL_OK;}
int main(void){
 g_can_healthy=1;
 /* Stop times out and clears the peripheral's BusOff status while HAL is ERROR.
    Recovery must inspect HAL state rather than treating cleared BusOff as healthy. */
 bus_off=1;stop_timeout=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR);
 assert(handle.State==HAL_FDCAN_STATE_ERROR&&g_can_recovering);
 bus_off=0;now=100;
 assert(can_bus_recover_if_bus_off()==HAL_OK);
 assert(handle.State==HAL_FDCAN_STATE_BUSY&&inits==1&&starts==1&&notifications==1&&!g_can_recovering&&can_bus_health_ok());
 /* A second fault is independently recoverable without SDK or board reset. */
 now=200;bus_off=1;assert(can_bus_recover_if_bus_off()==HAL_OK);
 assert(starts==2&&resets==2);
 /* Failed Init/filter/IRQ restoration must keep admission closed and retry. */
 now=300;handle.State=HAL_FDCAN_STATE_ERROR;bus_off=0;fail_init=1;
 assert(can_bus_recover_if_bus_off()==HAL_ERROR && g_can_recovering && !can_bus_health_ok());
 now=350;fail_init=0;assert(can_bus_recover_if_bus_off()==HAL_BUSY);
 now=400;fail_filter=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR && g_can_recovering);
 now=500;fail_filter=0;fail_notify=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR && g_can_recovering && !can_bus_health_ok());
 now=600;fail_notify=0;assert(can_bus_recover_if_bus_off()==HAL_OK && !g_can_recovering && can_bus_health_ok());
 /* A stopped READY controller also receives filters, Start and IRQ restoration. */
 now=700;handle.State=HAL_FDCAN_STATE_READY;assert(can_bus_recover_if_bus_off()==HAL_OK && !g_can_recovering && can_bus_health_ok());
 puts("Stop ERROR with cleared BusOff: transport recovered without power cycle");return 0;}
'''
  with tempfile.TemporaryDirectory()as directory:
   c=Path(directory)/'recovery.c';exe=Path(directory)/'recovery';c.write_text(code);subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(ROOT/'Core/Inc'),str(c),'-o',str(exe)],check=True);subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
