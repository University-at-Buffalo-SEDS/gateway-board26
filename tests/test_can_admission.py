import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class CanAdmissionTests(unittest.TestCase):
    def test_ring_preserves_control_capacity_and_packet_order(self):
        source = (ROOT / 'Core/Src/can_bus.c').read_text()
        ring = source[source.index('static inline uint16_t rb_next('):source.index('// =========================\n// Reassembly state')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "can_rx_admission.h"
#define CAN_BUS_RX_RING_DEPTH 48U
#define __DMB() ((void)0)
typedef struct { uint32_t std_id; uint8_t len, data[64]; } can_bus_rx_frame_t;
static can_bus_rx_frame_t g_rx_ring[48];
static uint16_t g_rx_head, g_rx_tail;
static uint32_t g_rx_dropped_frames, g_can_rx_bulk_dropped, g_can_rx_peer_dropped;
static uint32_t g_can_rx_sender_frames[8], g_can_rx_sender_dropped[8];
''' + ring + r'''
int main(void) {
 uint8_t bulk[16]={0,1,118}, status[16]={0,1,117}, fragment[64]={83,68,7,1,0,1,3,16,0};
 memcpy(fragment+9,bulk,16);
 assert(can_rx_is_bulk_loadcell(bulk,16));
 assert(can_rx_is_bulk_loadcell(fragment,64));
 fragment[5]=2; assert(!can_rx_is_bulk_loadcell(fragment,64)); fragment[5]=1;
 fragment[7]=64; assert(!can_rx_is_bulk_loadcell(fragment,64)); fragment[7]=16;
 uint8_t wrapped[32]={83,68,84,1,1}; memcpy(wrapped+5,bulk,16);
 assert(can_rx_is_bulk_loadcell(wrapped,21));
 wrapped[3]=2; assert(!can_rx_is_bulk_loadcell(wrapped,21));
 memset(wrapped+4,255,28); wrapped[3]=1; assert(!can_rx_is_bulk_loadcell(wrapped,32));
 assert(!can_rx_is_bulk_loadcell(status,16));
 assert(!can_rx_is_bulk_loadcell(NULL,16));
 for(unsigned n=0;n<7;n++) assert(!can_rx_is_bulk_loadcell(bulk,n));
 bulk[0]=64; assert(!can_rx_is_bulk_loadcell(bulk,16)); bulk[0]=0;
 // Repeated saturation and wraparound: 39 bulk slots, eight protected slots.
 for(unsigned pass=0;pass<1000;pass++) {
   for(unsigned i=0;i<39;i++) rb_push(0x180+i,bulk,16);
   rb_push(999,bulk,16);
   for(unsigned i=39;i<47;i++) rb_push(i,status,16);
   rb_push(999,status,16);
   can_bus_rx_frame_t out;
   for(unsigned i=0;i<47;i++) {assert(rb_pop(&out)); assert(out.std_id==(i<39 ? 0x180+i : i));}
   assert(!rb_pop(&out));
 }
 assert(g_can_rx_bulk_dropped==1000 && g_rx_dropped_frames==2000);
 // Previously unknown compact/fragmented DAQ traffic filled all 47 slots.
 // Full-ring shedding displaces the largest sender for a quiet board.
 uint8_t compact[16]={83,68,84,2,1};
 fragment[5]=3; fragment[7]=128; memcpy(fragment+9,status,16);
 unsigned before_ab=g_can_rx_sender_frames[5], before_ab_drops=g_can_rx_sender_dropped[5];
 for(unsigned pass=0;pass<1000;pass++) {
   for(unsigned i=0;i<47;i++) rb_push(0x107,compact,16);
   for(unsigned i=0;i<47;i++) rb_push(0x106,compact,16);
   for(unsigned i=0;i<3;i++) rb_push(0x105,fragment,64);
   can_bus_rx_frame_t out;
   unsigned daq=0, valve=0, actuator=0;
   for(unsigned i=0;i<47;i++) {
     assert(rb_pop(&out));
     if(out.std_id==0x107) daq++;
     else if(out.std_id==0x106) valve++;
     else { assert(out.std_id==0x105); actuator++; }
   }
   assert(daq==22 && valve==22 && actuator==3);
   assert(!rb_pop(&out));
 }
 assert(g_can_rx_peer_dropped==19000); // 8,000 earlier DAQ drops avoid full-ring shifting
 assert(g_can_rx_sender_frames[5]-before_ab==3000);
 assert(g_can_rx_sender_dropped[5]==before_ab_drops);
 assert(g_can_rx_sender_dropped[7]==25000 && g_can_rx_sender_dropped[6]==25000);
 // Opaque compact DAQ frames must leave eight slots for quiet peers.
 // Previously only recognizable full loadcell packets honored this reserve.
 for(unsigned i=0;i<47;i++) rb_push(0x107,compact,16);
 assert(g_rx_head!=g_rx_tail);
 can_bus_rx_frame_t saved=g_rx_ring[g_rx_tail];
 // A receive interrupt may occur while foreground is copying this tail slot.
 rb_push(0x105,fragment,64);
 assert(!memcmp(&saved,&g_rx_ring[g_rx_tail],sizeof(saved)));
 can_bus_rx_frame_t drain;
 for(unsigned i=0;i<39;i++) {assert(rb_pop(&drain)); assert(drain.std_id==0x107);}
 assert(rb_pop(&drain) && drain.std_id==0x105 && !rb_pop(&drain));
 // The dominant actuator's opaque ACK/status/schema fragments must survive.
 for(unsigned i=0;i<24;i++) rb_push(0x105,compact,16);
 for(unsigned i=0;i<15;i++) rb_push(0x107,compact,16);
 for(unsigned i=0;i<8;i++) rb_push(0x106,compact,16);
 unsigned critical_drops=g_can_rx_sender_dropped[5];
 for(unsigned i=0;i<3;i++) rb_push(0x005,fragment,64);
 assert(g_can_rx_sender_dropped[5]==critical_drops+3);
 // More bulk traffic cannot evict any of those three protected frames.
 for(unsigned i=0;i<47;i++) rb_push(0x107,compact,16);
 unsigned protected=0;
 while(rb_pop(&drain)) if(drain.std_id==0x005) protected++;
 assert(protected==3);
 // Priority is trusted even if a payload happens to look like bulk data.
 for(unsigned i=0;i<39;i++) rb_push(0x107,compact,16);
 rb_push(0x005,bulk,16);
 protected=0;
 while(rb_pop(&drain)) if(drain.std_id==0x005) protected++;
 assert(protected==1);
 // Capacity is recovered immediately after consumption, including wraparound.
 for(unsigned i=0;i<16;i++) rb_push(0x107,compact,16);
 can_bus_rx_frame_t out;
 for(unsigned i=0;i<16;i++) assert(rb_pop(&out));
 rb_push(0x107,compact,16); assert(rb_pop(&out));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)
            (path/'sedsnet_config.h').write_text('#define SEDS_DT_KG1000 118U\n#define SEDS_DT_KG50 119U\n')
            (path/'can_rx_admission.h').write_text((ROOT/'Core/Inc/can_rx_admission.h').read_text())
            exe=str(path/'admission')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',tmp,'-x','c','-','-o',exe],input=code,text=True,check=True)
            subprocess.run([exe],check=True)
