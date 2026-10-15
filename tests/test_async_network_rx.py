"""Wire callbacks copy into network RX storage; foreground processing is separate."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AsyncReceiveTests(unittest.TestCase):
    def test_handoff_copies_wire_and_does_not_dispatch_in_can_callback(self):
        source=(ROOT/"Core/Src/telemetry.c").read_text()
        start=source.index("volatile uint32_t g_gateway_can_rx_handoff_fail")
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
static unsigned queued, delivered, errors,g_telemetry_discovery_seen,admission_rejected;
static size_t seds_memory_admission_rejected(void){return admission_rejected;}
static uint8_t owned[128];static size_t owned_len;
static int init_telemetry_router(void){return 0;}
static void telemetry_lock(void){}
static void telemetry_unlock(void){}
static void telemetry_signal_deserialize_failure(void){errors++;}
static int seds_router_rx_packed_packet_to_queue_from_side(void *r,unsigned side,const uint8_t *p,size_t n){
 assert(r && side==2 && n<=sizeof(owned));if(fail){if(fail==2)admission_rejected++;return -1;}
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
 assert(g_gateway_can_rx_handoff_fail==1 && g_gateway_can_rx_handoff_last==-1);
 assert(g_gateway_can_queue_fail==1 && g_gateway_can_admission_fail==0);
 fail=2;rx_asynchronous(wire,4);
 assert(g_gateway_can_queue_fail==1 && g_gateway_can_admission_fail==1);
 fail=0;g_can_side_id=-1;rx_asynchronous(wire,4);assert(queued==2);
 rx_asynchronous(0,0);assert(queued==2);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/"handoff"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-function","-fsanitize=address,undefined","-x","c","-","-o",str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)

    def test_uart_handoff_owns_bytes_and_defers_routing(self):
        source=(ROOT/"Core/Src/telemetry.c").read_text()
        start=source.index("void telemetry_uart_handle_data")
        end=source.index("static uint8_t telemetry_timesync_is_source", start)
        code=r"""
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#define TELEMETRY_ENABLED 1
#define SEDS_OK 0
typedef int SedsResult;
static struct {void *r;} g_router={(void*)1};
static int side=2,fail;
static unsigned queued,success,errors,signals;
static uint8_t owned[128];
static int telemetry_uart_side_id(void){return side;}
static void sim_probe_observe_packed(const uint8_t*p,size_t n){(void)p;(void)n;}
static int seds_pkt_validate_packed(const uint8_t*p,size_t n){(void)p;(void)n;return 0;}
static void telemetry_uart_note_deserialize_result(unsigned ok){if(ok)success++;else errors++;}
static void telemetry_signal_deserialize_failure(void){signals++;}
static int log_error_asynchronous(const char*s,...){(void)s;return 0;}
static int print_telemetry_error(int n){return n;}
static int init_telemetry_router(void){return 0;}
static size_t seds_memory_admission_rejected(void){return 0;}
static void telemetry_lock(void){}
static void telemetry_unlock(void){}
static int seds_router_rx_packed_packet_to_queue_from_side(void*r,unsigned id,const uint8_t*p,size_t n){
 assert(r && id==2 && n<=sizeof(owned));if(fail)return -1;
 memcpy(owned,p,n);queued++;return 0;
}
static int seds_router_rx_packed_packet_to_queue(void*r,const uint8_t*p,size_t n){return seds_router_rx_packed_packet_to_queue_from_side(r,2,p,n);}
static int seds_router_receive_packed_from_side(void*r,unsigned id,const uint8_t*p,size_t n){(void)r;(void)id;(void)p;(void)n;abort();}
static int seds_router_receive_packed(void*r,const uint8_t*p,size_t n){(void)r;(void)p;(void)n;abort();}
"""+source[start:end]+r"""
int main(void){
 uint8_t wire[]={1,2,3,4};telemetry_uart_handle_data(wire,4);
 assert(queued==1 && success==1 && !errors);
 memset(wire,0,4);assert(owned[0]==1 && owned[3]==4);
 fail=1;telemetry_uart_handle_data(wire,4);
 assert(queued==1 && success==1 && errors==1 && signals==1);
 fail=0;side=-1;telemetry_uart_handle_data(wire,4);assert(queued==2);
 telemetry_uart_handle_data(NULL,4);telemetry_uart_handle_data(wire,0);assert(queued==2);
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/"uart-handoff"
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror","-Wno-unused-function","-fsanitize=address,undefined","-x","c","-","-o",str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)
