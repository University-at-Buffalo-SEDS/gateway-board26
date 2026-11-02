from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class CanRecoveryTests(unittest.TestCase):
 def test_error_passive_stall_completion_progress_and_failed_restart(self):
  source=(ROOT/'Core/Src/can_bus.c').read_text()
  start=source.index('volatile uint32_t g_fdcan_tx_complete_events;')
  end=source.index('static can_bus_rx_frame_t g_rx_ring',start)
  callbacks=source[source.index('void HAL_FDCAN_TxFifoEmptyCallback('):source.index('// Call this periodically from thread/main-loop context.')]
  code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include "can_transport_health.h"
#define HAL_OK 0
#define HAL_ERROR 1
#define HAL_BUSY 2
#define HAL_FDCAN_STATE_BUSY 2
#define HAL_FDCAN_STATE_READY 1
#define HAL_FDCAN_STATE_ERROR 3
#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE 1
#define FDCAN_IT_RX_FIFO1_NEW_MESSAGE 2
#define FDCAN_IT_RX_FIFO0_MESSAGE_LOST 4
#define FDCAN_IT_RX_FIFO1_MESSAGE_LOST 8
#define FDCAN_IT_TX_FIFO_EMPTY 16
#define FDCAN_IT_TX_COMPLETE 32
#define CAN_BUS_POLLING 0
#define FDCAN_TX_BUFFER0 1U
#define FDCAN_TX_BUFFER1 2U
#define FDCAN_TX_BUFFER2 4U
typedef int HAL_StatusTypeDef;
typedef struct {uint32_t TXBRP,PSR,ECR;} Registers;
typedef struct {Registers *Instance;int State;} FDCAN_HandleTypeDef;
typedef struct {uint32_t BusOff;} FDCAN_ProtocolStatusTypeDef;
static FDCAN_HandleTypeDef *g_hfdcan;
static uint32_t tick, busoff, stopped, restarted, reset, pumps;
static int fail_stop,fail_start,fail_status;
static uint32_t g_fdcan_bus_off_count,g_fdcan_recovery_count;
static uint32_t HAL_GetTick(void){return tick;}
static int HAL_FDCAN_GetProtocolStatus(FDCAN_HandleTypeDef *h,FDCAN_ProtocolStatusTypeDef *p){(void)h;p->BusOff=busoff;return fail_status;}
static int HAL_FDCAN_AbortTxRequest(FDCAN_HandleTypeDef *h,uint32_t b){assert(b==7);h->Instance->TXBRP=0;return 0;}
static int HAL_FDCAN_Stop(FDCAN_HandleTypeDef *h){(void)h;stopped++;if(fail_stop){h->State=HAL_FDCAN_STATE_ERROR;return HAL_ERROR;}h->State=HAL_FDCAN_STATE_READY;return HAL_OK;}
static int HAL_FDCAN_Start(FDCAN_HandleTypeDef *h){(void)h;restarted++;assert(h->State==HAL_FDCAN_STATE_READY);if(fail_start)return HAL_ERROR;h->State=HAL_FDCAN_STATE_BUSY;return HAL_OK;}
static int HAL_FDCAN_Init(FDCAN_HandleTypeDef *h){h->State=HAL_FDCAN_STATE_READY;return HAL_OK;}
static int HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef *h,unsigned mask,unsigned buffers){assert(h->State==HAL_FDCAN_STATE_BUSY&&mask==63&&buffers==7);return HAL_OK;}
static void can_tx_queue_reset(void){reset++;}
static void can_tx_queue_pump(void){pumps++;}
''' +source[start:end]+callbacks+r'''
static HAL_StatusTypeDef can_bus_configure_filters(FDCAN_HandleTypeDef *h){assert(h->State==HAL_FDCAN_STATE_READY);return HAL_OK;}

int main(void){
 Registers r={7,0x77b,0xff7f85};FDCAN_HandleTypeDef h={&r,HAL_FDCAN_STATE_BUSY},other={&r,HAL_FDCAN_STATE_BUSY};g_hfdcan=&h;g_can_healthy=1;
 // Captured failure: all three buffers occupied, ACK error, error passive, no bus-off.
 assert(can_bus_recover_if_bus_off()==HAL_OK && !stopped);
 tick=999;assert(can_bus_recover_if_bus_off()==HAL_OK && !stopped);
 tick=1000;assert(can_bus_recover_if_bus_off()==HAL_OK && stopped==1 && restarted==1 && reset==1);
 assert(g_fdcan_stall_recovery_count==1 && g_fdcan_recovery_count==1 && can_bus_health_ok());
 assert(g_fdcan_last_stall_psr==0x77b && g_fdcan_last_stall_ecr==0xff7f85);
 // Completion interrupts count only this peripheral. Continuous successful TX never trips.
 r.TXBRP=7;
 for(unsigned i=0;i<100;i++){tick+=900;HAL_FDCAN_TxBufferCompleteCallback(&other,7);HAL_FDCAN_TxBufferCompleteCallback(&h,0);unsigned n=g_fdcan_tx_complete_events;HAL_FDCAN_TxBufferCompleteCallback(&h,7);assert(g_fdcan_tx_complete_events==n+1);assert(can_bus_recover_if_bus_off()==HAL_OK);}
 assert(stopped==1);
 // No pending traffic is healthy, including wraparound.
 r.TXBRP=0;tick=UINT32_MAX-100;assert(can_bus_recover_if_bus_off()==HAL_OK);
 r.TXBRP=7;assert(can_bus_recover_if_bus_off()==HAL_OK);tick=898;assert(can_bus_recover_if_bus_off()==HAL_OK && stopped==1);tick=899;assert(can_bus_recover_if_bus_off()==HAL_OK && stopped==2);
 // Bus-off is recovered immediately, rather than waiting for the deadline.
 busoff=1;assert(can_bus_recover_if_bus_off()==HAL_OK && stopped==3 && g_fdcan_bus_off_count==1);busoff=0;
 // Interrupt cannot refill hardware while recovery is in progress.
 g_can_recovering=1;HAL_FDCAN_TxFifoEmptyCallback(&h);assert(!pumps);g_can_recovering=0;HAL_FDCAN_TxFifoEmptyCallback(&h);assert(pumps==1);
 busoff=1;fail_stop=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR && !can_bus_health_ok() && g_can_recovering);
 tick+=100;fail_stop=0;fail_start=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR && !can_bus_health_ok());
 tick+=100;fail_start=0;assert(can_bus_recover_if_bus_off()==HAL_OK && can_bus_health_ok() && !g_can_recovering); // checked restart restores health without board reset
 assert(g_fdcan_recovery_fail_count==2);
 fail_status=1;assert(can_bus_recover_if_bus_off()==HAL_ERROR);
}
'''
  with tempfile.TemporaryDirectory() as d:
   exe=Path(d)/'test'
   subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',str(ROOT/'Core/Inc'),'-x','c','-','-o',str(exe)],input=code,text=True,check=True)
   subprocess.run([str(exe)],check=True)
if __name__=='__main__':unittest.main()
