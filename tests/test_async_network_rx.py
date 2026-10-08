"""CAN callbacks copy into network RX storage; foreground processing is separate."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AsyncReceiveTests(unittest.TestCase):
    def test_handoff_copies_wire_and_does_not_dispatch_in_can_callback(self):
        source=(ROOT/"Core/Src/telemetry.c").read_text()
        start=source.index("void rx_asynchronous(")
        end=source.index("static UNUSED_FUNCTION void rx_synchronous",start)
        code=r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#define TELEMETRY_ENABLED 1
#define SEDS_OK 0
#define SEDS_ERR -1
typedef int SedsResult;
static struct { void *r; } g_router={(void*)1};
static int g_can_side_id=2, fail;
static unsigned queued, delivered, errors,g_telemetry_discovery_seen;
static uint8_t owned[128];static size_t owned_len;
static int init_telemetry_router(void){return 0;}
static void telemetry_lock(void){}
static void telemetry_unlock(void){}
static void telemetry_signal_deserialize_failure(void){errors++;}
static int seds_router_rx_packed_packet_to_queue_from_side(void *r,unsigned side,const uint8_t *p,size_t n){
 assert(r && side==2 && n<=sizeof(owned));if(fail)return -1;
 memcpy(owned,p,n);owned_len=n;queued++;return 0;
}
static int seds_router_rx_packed_packet_to_queue(void *r,const uint8_t *p,size_t n){return seds_router_rx_packed_packet_to_queue_from_side(r,2,p,n);}
static int seds_router_receive_packed_from_side(void *r,unsigned side,const uint8_t *p,size_t n){(void)r;(void)side;(void)p;(void)n;abort();}
static int seds_router_receive_packed(void *r,const uint8_t *p,size_t n){(void)r;(void)p;(void)n;abort();}
"""+source[start:end]+r"""
int main(void){
 uint8_t wire[4]={1,2,3,4};rx_asynchronous(wire,4);
 assert(queued==1 && delivered==0 && owned_len==4);
 memset(wire,0,4);assert(owned[0]==1 && owned[3]==4);
 // Rejecting queue pressure must remain observable and never fake success.
 g_telemetry_discovery_seen=0;fail=1;rx_asynchronous(wire,4);
 assert(queued==1 && errors==1 && !g_telemetry_discovery_seen);
 fail=0;g_can_side_id=-1;rx_asynchronous(wire,4);assert(queued==2);
 rx_asynchronous(0,0);assert(queued==2);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/"handoff"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-function","-fsanitize=address,undefined","-x","c","-","-o",str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)
