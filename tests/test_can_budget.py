import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class CanBudgetTests(unittest.TestCase):
    def test_sustained_ingress_yields_and_preserves_partial_work(self):
        source = (ROOT / "Core/Src/can_bus.c").read_text()
        start = source.index("uint32_t can_bus_process_rx_for(")
        end = source.index("uint32_t can_bus_rx_dropped_frames", start)
        code = r"""
#include <assert.h>
#include <stdint.h>
#define CAN_BUS_POLLING 0
#define CAN_BUS_RX_RING_DEPTH 48U
typedef struct { unsigned id; } can_bus_rx_frame_t;
static unsigned available, handled, tick, next_id;
static uint16_t g_rx_head, g_rx_tail;
static int continuous;
static uint32_t HAL_GetTick(void) { return tick++; }
static void reasm_expire_old(uint32_t now) { (void)now; }
static void can_tx_queue_service(void) {}
static int can_bus_recover_if_bus_off(void) { return 0; }
static int rb_pop(can_bus_rx_frame_t *f) {
 if (!available && !continuous) return 0;
 if (available) available--;
 f->id = next_id++; return 1;
}
static void handle_rx_frame(can_bus_rx_frame_t *f, uint32_t now) {
 assert(f->id == handled++); assert(now + 1 == tick);
}
""" + source[start:end] + r"""
int main(void) {
 continuous=1;
 assert(can_bus_process_rx_budget(8)==8 && handled==8);
 // Even permanent ingress cannot exceed the ring-size ceiling.
 assert(can_bus_process_rx_budget(1000)==48 && handled==56);
 assert(can_bus_process_rx_budget(0)==0 && handled==56);
 continuous=0; available=11;
 assert(can_bus_process_rx_budget(8)==8 && available==3);
 assert(can_bus_process_rx_budget(8)==3 && available==0);
 assert(can_bus_process_rx_budget(8)==0);
 available=60; can_bus_process_rx(); assert(available==12);
 unsigned before=handled;
 // A time-bounded slice must yield even when the frame budget is not full.
 assert(can_bus_process_rx_for(8,2)==1 && handled==before+1);
 before=handled; tick=UINT32_MAX-1;
 assert(can_bus_process_rx_for(8,2)==1 && handled==before+1);
 g_rx_head=10; g_rx_tail=3; assert(can_bus_rx_pending()==7);
 g_rx_head=3; g_rx_tail=46; assert(can_bus_rx_pending()==5);
 g_rx_head=g_rx_tail; assert(can_bus_rx_pending()==0);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe = str(Path(tmp) / "budget")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", exe], input=code, text=True, check=True)
            subprocess.run([exe], check=True)
