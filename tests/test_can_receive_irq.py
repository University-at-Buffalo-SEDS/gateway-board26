import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, signature):
    start = source.index(signature)
    pos = source.index('{', start)
    depth = 1
    end = pos + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


class CanReceiveInterruptTests(unittest.TestCase):
    def test_interrupt_admission_loss_reporting_and_initialization_order(self):
        source = (ROOT / 'Core/Src/can_bus.c').read_text()
        functions = '\n'.join(function(source, signature) for signature in (
            'void can_bus_init(', 'void HAL_FDCAN_RxFifo0Callback(',
            'void HAL_FDCAN_RxFifo1Callback('))
        harness = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef struct { int State; } FDCAN_HandleTypeDef;
#define HAL_FDCAN_STATE_BUSY 1
#define HAL_OK 0
#define CAN_BUS_POLLING 0
#define CAN_BUS_REASM_SLOTS 2
#define FDCAN_IT_RX_FIFO0_NEW_MESSAGE 1U
#define FDCAN_IT_RX_FIFO1_NEW_MESSAGE 2U
#define FDCAN_IT_RX_FIFO0_MESSAGE_LOST 4U
#define FDCAN_IT_RX_FIFO1_MESSAGE_LOST 8U
#define FDCAN_RX_FIFO0 0U
#define FDCAN_RX_FIFO1 1U
static FDCAN_HandleTypeDef *g_hfdcan;
static unsigned g_rx_head, g_rx_tail, g_rx_dropped_frames;
static unsigned g_fdcan_rx_hw_overflow_count, g_fdcan_init_error_count;
static unsigned g_can_reasm_completed, g_can_reasm_seq_resets, g_can_reasm_slot_evictions;
static unsigned g_can_reasm_expired, g_can_reasm_param_mismatch;
static int g_reasm[2], notification_failure, starts;
static unsigned notifications, drains[2];
static int stops, stop_failure;
static int HAL_FDCAN_Stop(FDCAN_HandleTypeDef *h) { stops++; assert(h->State==HAL_FDCAN_STATE_BUSY); return stop_failure; }
static int can_bus_configure_filters(FDCAN_HandleTypeDef *h) { (void)h; return 0; }
static int HAL_FDCAN_ActivateNotification(FDCAN_HandleTypeDef *h, unsigned flags, unsigned x) {
 (void)h; (void)x; notifications=flags; return notification_failure;
}
static void reasm_reset(int *r) { *r=0; }
static int HAL_FDCAN_Start(FDCAN_HandleTypeDef *h) {
 (void)h; starts++; assert(g_rx_head==0 && g_rx_tail==0);
 // A reception can occur as soon as the peripheral starts.
 g_rx_head=1; return 0;
}
static void can_bus_drain_rx_fifo(FDCAN_HandleTypeDef *h, unsigned fifo) { assert(h==g_hfdcan); drains[fifo]++; }
'''
        main = r'''
int main(void) {
 FDCAN_HandleTypeDef h={0}, other={0};
 g_rx_head=99; g_rx_tail=88; can_bus_init(&h);
 assert(starts==1 && g_rx_head==1 && notifications==15 && stops==0);
 HAL_FDCAN_RxFifo0Callback(&h,1); assert(drains[0]==1 && !g_fdcan_rx_hw_overflow_count);
 HAL_FDCAN_RxFifo0Callback(&h,4); assert(drains[0]==2 && g_fdcan_rx_hw_overflow_count==1);
 HAL_FDCAN_RxFifo1Callback(&h,2|8); assert(drains[1]==1 && g_fdcan_rx_hw_overflow_count==2);
 HAL_FDCAN_RxFifo1Callback(&h,0); assert(drains[1]==1);
 HAL_FDCAN_RxFifo0Callback(&other,1|4); assert(drains[0]==2 && g_fdcan_rx_hw_overflow_count==2);
 notification_failure=1; can_bus_init(&h);
 assert(g_fdcan_init_error_count==1 && starts==1);
 notification_failure=0; h.State=HAL_FDCAN_STATE_BUSY; stop_failure=1; can_bus_init(&h);
 assert(stops==1 && g_fdcan_init_error_count==2 && starts==1);
 stop_failure=0; can_bus_init(&h); assert(stops==2 && starts==2);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)/'irq.c'
            path.write_text(harness+functions+main)
            executable = Path(tmp)/'irq'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(path),'-o',str(executable)],check=True)
            subprocess.run([str(executable)],check=True)
