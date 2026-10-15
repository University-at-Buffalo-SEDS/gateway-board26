from pathlib import Path
import subprocess
import tempfile
import unittest

class CanStartupBufferTests(unittest.TestCase):
    def test_startup_burst_and_controls_at_both_buffer_depths(self):
        r=Path(__file__).resolve().parents[1]
        s=(r/'Core/Src/can_bus.c').read_text()
        ring=s[s.index('static inline uint16_t rb_next('):s.index('// =========================\n// Reassembly state')]
        a=s.index('typedef struct {',s.index('// RX ring buffer'));frame=s[a:s.index('} can_bus_rx_frame_t;',a)+len('} can_bus_rx_frame_t;')]
        for depth in [48,192]:
            code='''
        #include <assert.h>
        #include <stdint.h>
        #include <string.h>
        #include "can_rx_admission.h"
        static uint32_t irq;
        static uint32_t __get_PRIMASK(void) { return irq; }
        static void __disable_irq(void) { irq=1; }
        static void __set_PRIMASK(uint32_t value) { irq=value; }
        #define __DMB() do {} while(0)
        '''+f'#define CAN_BUS_RX_RING_DEPTH {depth}U\n'+frame+'''
        _Static_assert(sizeof(can_bus_rx_frame_t)==68U,"frame layout");
        static can_bus_rx_frame_t g_rx_ring[CAN_BUS_RX_RING_DEPTH];
        static uint16_t g_rx_head,g_rx_tail;
        static void (*g_rx_wakeup_callback)(void);
        static uint32_t g_rx_dropped_frames,g_can_rx_bulk_dropped,g_can_rx_peer_dropped;
        static uint32_t g_can_rx_sender_frames[8],g_can_rx_sender_dropped[8];
        '''+ring+'''
        int main(void) {
         uint8_t compact[16]={83,68,84,2,1}, critical[16]={0,1,104};
         for(unsigned i=0;i<149;i++) rb_push(0x107,compact,16);
         for(unsigned i=0;i<8;i++) rb_push(0x005,critical,16);
         assert(g_rx_dropped_frames == (CAN_BUS_RX_RING_DEPTH==48 ? 110U : 0U));
         unsigned control=0; can_bus_rx_frame_t out;
         while(rb_pop(&out)) if(out.std_id==0x005) control++;
         assert(control==8);
         for(unsigned pass=0;pass<1000;pass++) {
           for(unsigned i=0;i<CAN_BUS_RX_RING_DEPTH-1;i++) rb_push(0x107,compact,16);
           for(unsigned i=0;i<8;i++) rb_push(0x005,critical,16);
           control=0; unsigned all=0; irq=pass&1U;
           while(rb_pop(&out)) { assert(irq==(pass&1U));all++;if(out.std_id==0x005) control++; }
           assert(control==8 && all==CAN_BUS_RX_RING_DEPTH-1 && irq==(pass&1U));
         }
         return 0;
        }
        '''
            with tempfile.TemporaryDirectory() as tmp:
                p=Path(tmp)
                (p/'sedsnet_config.h').write_text('#define SEDS_DT_KG1000 118U\n#define SEDS_DT_KG50 119U\n#define SEDS_DT_FUEL_TANK_PRESSURE 111U\n')
                (p/'can_rx_admission.h').write_text((r/'Core/Inc/can_rx_admission.h').read_text())
                exe=p/'startup'
                subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',tmp,'-x','c','-','-o',str(exe)],input=code,text=True,check=True)
                subprocess.run([str(exe)],check=True)
