import subprocess
import tempfile
import unittest
from pathlib import Path
ROOT = Path(__file__).resolve().parents[1]

class CanAdmissionTests(unittest.TestCase):
    def test_ring_preserves_control_capacity_and_packet_order(self):
        source = (ROOT / 'Core/Src/can_bus.c').read_text()
        ring = source[source.index('static inline uint16_t rb_next('):source.index('// =========================\n// Reassembly state')]
        frame_start = source.index('typedef struct {', source.index('// RX ring buffer'))
        frame = source[frame_start:source.index('} can_bus_rx_frame_t;', frame_start) + len('} can_bus_rx_frame_t;')]
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "can_rx_admission.h"
#define CAN_BUS_RX_RING_DEPTH 48U
/* Link format v2 identifiers (can_link.h): S = one whole packet, F = the
 * FIRST frame of a two-frame packet, UD/UC = data/control frames of a sender
 * token outside the known boards 1..7. */
#define S(base) can_link_id((base), CAN_LINK_SINGLE)
#define F(base) can_link_id((base), CAN_LINK_FIRST)
#define UD(i) (0x400U + 8U * (i))
#define UC(i) can_link_id(8U + ((i) % 8U), CAN_LINK_SINGLE)
/* Is this whole packet, framed as the boards frame it, bulk telemetry? */
static int bulkpkt(const uint8_t *p, size_t n) {
  uint8_t f[64]; uint32_t id; size_t take;
  const size_t w = can_link_frame(p, n, 0U, 0x107U, f, &id, &take);
  return can_rx_is_bulk_telemetry(id, f, w);
}
static uint32_t irq, consumer_expected;
static uint32_t __get_PRIMASK(void) { return irq; }
static void __disable_irq(void) { irq = 1; }
static void __set_PRIMASK(uint32_t mask) { irq = mask; }
#define __DMB() do { if (consumer_expected) assert(irq); } while (0)
''' + frame + r'''
_Static_assert(sizeof(can_bus_rx_frame_t) == 68U, "standard-ID RX storage must stay compact");
static can_bus_rx_frame_t g_rx_ring[48];
static uint16_t g_rx_head, g_rx_tail;
static unsigned wakes;
static void notify(void){assert(g_rx_head!=g_rx_tail);wakes++;}
static void (*g_rx_wakeup_callback)(void);
static uint32_t g_rx_dropped_frames, g_can_rx_bulk_dropped, g_can_rx_peer_dropped;
static uint32_t g_can_rx_sender_frames[8], g_can_rx_sender_dropped[8];
''' + ring + r'''
int main(void) {
 uint8_t full[64]; memset(full, 0x5a, sizeof(full));
 can_bus_rx_frame_t exact;
 g_rx_wakeup_callback=notify;
 rb_push(0x7ff, full, sizeof(full)); assert(wakes==1); consumer_expected=1;
 assert(rb_pop(&exact)); consumer_expected=0;
 assert(exact.std_id==0x7ff && exact.len==64 && memcmp(exact.data,full,64)==0);
 uint8_t bulk[16]={0,1,118}, status[16]={0,1,117}, fragment[64];
 memset(fragment,0x33,sizeof fragment); memcpy(fragment,bulk,16);
 assert(bulkpkt(bulk,16) && can_rx_is_bulk_telemetry(S(0x107),bulk,16));
 assert(bulkpkt(bulk,13));                       /* padded SINGLE frame */
 /* Only a SINGLE frame holds a whole packet: a FIRST frame is never shed
  * by type, and damaged padding is not parsed. */
 assert(!can_rx_is_bulk_telemetry(F(0x107),fragment,64));
 {uint8_t f[64];uint32_t id;size_t t;size_t w=can_link_frame(bulk,13,0,0x107,f,&id,&t);
  f[w-1]=0;assert(!can_rx_is_bulk_telemetry(id,f,w));}
 uint8_t wrapped[32]={83,68,84,1,1}; memcpy(wrapped+5,bulk,16);
 assert(bulkpkt(wrapped,21));
 wrapped[3]=2; assert(!bulkpkt(wrapped,21));
 memset(wrapped+4,255,28); wrapped[3]=1; assert(!bulkpkt(wrapped,32));
 assert(!bulkpkt(status,16));
 assert(!can_rx_is_bulk_telemetry(S(0x107),NULL,16));
 for(unsigned n=0;n<7;n++) assert(!bulkpkt(bulk,n));
 bulk[0]=64; assert(!bulkpkt(bulk,16)); bulk[0]=0;
 // Repeated saturation and wraparound: 39 bulk slots, eight protected slots.
 for(unsigned pass=0;pass<1000;pass++) {
   for(unsigned i=0;i<39;i++) rb_push(UD(i),bulk,16);
   rb_push(UD(99),bulk,16);
   for(unsigned i=39;i<47;i++) rb_push(UC(i),status,16);
   rb_push(UD(99),status,16);
   can_bus_rx_frame_t out;
   for(unsigned i=0;i<47;i++) {assert(rb_pop(&out)); assert(out.std_id==(i<39 ? UD(i) : UC(i)));}
   assert(!rb_pop(&out));
 }
 assert(g_can_rx_bulk_dropped==1000 && g_rx_dropped_frames==2000);
 // Previously unknown compact/fragmented DAQ traffic filled all 47 slots.
 // Full-ring shedding displaces the largest sender for a quiet board.
 uint8_t compact[16]={83,68,84,2,1};
 memcpy(fragment,status,16);
 unsigned before_ab=g_can_rx_sender_frames[5], before_ab_drops=g_can_rx_sender_dropped[5];
 for(unsigned pass=0;pass<1000;pass++) {
   for(unsigned i=0;i<47;i++) rb_push(S(0x107),compact,16);
   for(unsigned i=0;i<47;i++) rb_push(S(0x106),compact,16);
   for(unsigned i=0;i<3;i++) rb_push(F(0x105),fragment,64);
   can_bus_rx_frame_t out;
   unsigned daq=0, valve=0, actuator=0;
   for(unsigned i=0;i<47;i++) {
     assert(rb_pop(&out));
     if(out.std_id==S(0x107)) daq++;
     else if(out.std_id==S(0x106)) valve++;
     else { assert(out.std_id==F(0x105)); actuator++; }
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
 for(unsigned i=0;i<47;i++) rb_push(S(0x107),compact,16);
 assert(g_rx_head!=g_rx_tail);
 can_bus_rx_frame_t saved=g_rx_ring[g_rx_tail];
 // A receive interrupt may occur while foreground is copying this tail slot.
 rb_push(F(0x105),fragment,64);
 assert(!memcmp(&saved,&g_rx_ring[g_rx_tail],sizeof(saved)));
 can_bus_rx_frame_t drain;
 for(unsigned i=0;i<39;i++) {assert(rb_pop(&drain)); assert(drain.std_id==S(0x107));}
 assert(rb_pop(&drain) && drain.std_id==F(0x105) && !rb_pop(&drain));
 // The dominant actuator's opaque ACK/status/schema fragments must survive.
 for(unsigned i=0;i<24;i++) rb_push(S(0x105),compact,16);
 for(unsigned i=0;i<15;i++) rb_push(S(0x107),compact,16);
 for(unsigned i=0;i<8;i++) rb_push(S(0x106),compact,16);
 unsigned critical_drops=g_can_rx_sender_dropped[5];
 for(unsigned i=0;i<3;i++) rb_push(F(0x005),fragment,64);
 assert(g_can_rx_sender_dropped[5]==critical_drops+3);
 // More bulk traffic cannot evict any of those three protected frames.
 for(unsigned i=0;i<47;i++) rb_push(S(0x107),compact,16);
 unsigned protected=0;
 while(rb_pop(&drain)) if(drain.std_id==F(0x005)) protected++;
 assert(protected==3);
 // Priority is trusted even if a payload happens to look like bulk data.
 for(unsigned i=0;i<39;i++) rb_push(S(0x107),compact,16);
 rb_push(S(0x005),bulk,16);
 protected=0;
 while(rb_pop(&drain)) if(drain.std_id==S(0x005)) protected++;
 assert(protected==1);
 // Capacity is recovered immediately after consumption, including wraparound.
 for(unsigned i=0;i<16;i++) rb_push(S(0x107),compact,16);
 can_bus_rx_frame_t out;
 for(unsigned i=0;i<16;i++) assert(rb_pop(&out));
 rb_push(S(0x107),compact,16); assert(rb_pop(&out)); uint8_t pressure[16]={0,1,111}, battery[16]={0,1,104};
 assert(bulkpkt(pressure,16));
 assert(!bulkpkt(battery,16));
 // Pressure bursts must also leave slots for infrequent battery reports.
 for(unsigned i=0;i<47;i++) rb_push(S(0x106),pressure,16);
 for(unsigned i=0;i<8;i++) rb_push(S(0x106),battery,16);
 can_bus_rx_frame_t power;
 for(unsigned i=0;i<39;i++) {assert(rb_pop(&power));assert(power.data[2]==111);}
 for(unsigned i=0;i<8;i++) {assert(rb_pop(&power));assert(power.data[2]==104);}
 assert(!rb_pop(&power));
 // Protected metadata uses its own arbitration band while retaining the
 // same peer accounting and full-ring displacement of ordinary telemetry.
 const unsigned old_schema_drops=g_can_rx_sender_dropped[7];
 for(unsigned i=0;i<47;i++) rb_push(S(0x106),compact,16);
 rb_push(S(0x087),full,64);
 unsigned schemas=0,ordinary=0;
 while(rb_pop(&power)) {if(power.std_id==S(0x087))schemas++;else ordinary++;}
 assert(schemas==1 && ordinary==46);
 assert(g_can_rx_sender_dropped[7]==old_schema_drops);
 assert(can_rx_sender_index(S(0x087))==7 && can_rx_sender_index(F(0x107))==7);
 assert(can_rx_sender_index(UD(1))==0 && can_rx_sender_index(UC(3))==11); // outside the known boards
 // Empty and occupied ring consumption must restore the caller's IRQ mask.
 assert(!irq); irq=1; assert(!rb_pop(&power) && irq);
 rb_push(S(0x006),battery,16); assert(rb_pop(&power) && irq); irq=0;
 assert(!rb_pop(&power) && !irq);

}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path=Path(tmp)
            (path/'sedsnet_config.h').write_text('#define SEDS_DT_KG1000 118U\n#define SEDS_DT_KG50 119U\n#define SEDS_DT_OX_TANK_PRESSURE 111U\n')
            (path/'can_rx_admission.h').write_text((ROOT/'Core/Inc/can_rx_admission.h').read_text())
            (path/'can_link.h').write_text((ROOT/'Core/Inc/can_link.h').read_text())
            exe=str(path/'admission')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-I',tmp,'-x','c','-','-o',exe],input=code,text=True,check=True)
            subprocess.run([exe],check=True)


class RealTypeIdAdmissionTests(unittest.TestCase):
    """The bulk check against the board's real hashed type IDs (u32 ULEB128)."""

    def test_full_type_ids_classify_bulk_and_never_control(self):
        import re
        header = (ROOT / 'Core/Inc/sedsnet_config.h').read_text()
        types = re.findall(r'#define (SEDS_DT_\w+) \(\(SedsDataType\)(\d+)U\)', header)
        self.assertGreater(len(types), 20)
        bulk = {'SEDS_DT_KG1000', 'SEDS_DT_KG50', 'SEDS_DT_OX_TANK_PRESSURE'}
        self.assertTrue(bulk <= {name for name, _ in types})
        cases = ''.join(f' check({name},{int(name in bulk)});\n' for name, _ in types)
        code = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "can_rx_admission.h"
static int bulkpkt(const uint8_t *p, size_t n) {
  uint8_t f[64]; uint32_t id; size_t take;
  const size_t w = can_link_frame(p, n, 0U, 0x107U, f, &id, &take);
  return can_rx_is_bulk_telemetry(id, f, w);
}
static size_t frame(uint8_t *f, uint32_t type){
 size_t n=0;f[n++]=0;f[n++]=1;
 do{uint8_t b=type&0x7fU;type>>=7;f[n++]=(uint8_t)(b|(type?0x80U:0U));}while(type);
 while(n<16)f[n++]=0x11;return n;
}
static void check(uint32_t type,int expected){
 uint8_t f[16];size_t n=frame(f,type);
 assert(bulkpkt(f,n)==expected);
 /* Reliable/unknown flags always protect the frame. */
 f[0]=0x02;assert(!bulkpkt(f,n));f[0]=0;
 /* The same frame inside an SDT envelope. */
 uint8_t w[32]={83,68,84,1,1};memcpy(w+5,f,n);
 assert(bulkpkt(w,5+n)==expected);
}
int main(void){
''' + cases + r'''
 /* The legacy FUEL_TANK_PRESSURE ID and IDs sharing only the first byte of
  * a bulk ID are not bulk. */
 check(79920485U,0);check((uint32_t)SEDS_DT_KG1000^0x100U,0);
 /* A type ULEB cut short by the frame end is never bulk. */
 uint8_t f[16];frame(f,(uint32_t)SEDS_DT_KG1000);
 for(size_t n=0;n<7;n++)assert(!bulkpkt(f,n));
 uint8_t over[16]={0,1,0xff,0xff,0xff,0xff,0x7f,0,0,0};
 assert(!bulkpkt(over,16));
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp)
            (path / 'sedsnet.h').write_text('#pragma once\n#include <stdint.h>\ntypedef uint32_t SedsDataType;\ntypedef uint32_t SedsDataEndpoint;\n')
            exe = str(path / 'real-ids')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-I', tmp, '-I', str(ROOT / 'Core/Inc'), '-x', 'c', '-', '-o', exe],
                           input=code, text=True, check=True)
            subprocess.run([exe], check=True)


if __name__ == '__main__':
    unittest.main()
