"""Reclaim old queued work before CAN admission, never replay the new packet."""
from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class ReceiveReclaimTests(unittest.TestCase):
    def test_low_memory_dispatches_old_work_before_the_single_handoff(self):
        source=(ROOT/'Core/Src/telemetry.c').read_text();start=source.index('volatile uint32_t g_gateway_can_rx_handoff_fail');end=source.index('static UNUSED_FUNCTION void rx_synchronous',start)
        code=r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#define TELEMETRY_ENABLED 1
#define TELEMETRY_USE_TLSF 1
#define SEDS_OK 0
#define SEDS_ERR -1
typedef int SedsResult;
static struct{void*r;}g_router={(void*)1};static int g_can_side_id=2;
static unsigned errors,admission_rejected,g_telemetry_discovery_seen;
static unsigned handoffs,queued,old_delivered,uart_runs,uart_frees,locked;
static size_t available=5572;
static uint32_t g_gateway_admission_last_free,g_gateway_admission_last_additional,g_gateway_admission_last_largest;
static uint8_t saved[49];
static size_t telemetry_tlsf_available_bytes(void){return available;}
static void telemetry_uart_process(void){assert(!locked);uart_runs++;if(uart_frees)available+=1024;}
static SedsResult process_all_queues_timeout(uint32_t ms){assert(!locked&&ms==1);old_delivered++;available+=4096;return SEDS_OK;}
static size_t seds_memory_admission_rejected(void){return admission_rejected;}
static int init_telemetry_router(void){return SEDS_OK;}
static void telemetry_lock(void){assert(!locked);locked=1;}
static void telemetry_unlock(void){assert(locked);locked=0;}
static void telemetry_signal_deserialize_failure(void){errors++;}
static int seds_router_rx_packed_packet_to_queue_from_side(void*r,unsigned side,const uint8_t*p,size_t n){
 assert(locked&&r&&side==2&&(n==4||n==49));handoffs++;
 if(available<(n==49?6291:5622)){admission_rejected++;return SEDS_ERR;}
 memcpy(saved,p,n);queued++;return SEDS_OK;
}
static int seds_router_rx_packed_packet_to_queue(void*r,const uint8_t*p,size_t n){return seds_router_rx_packed_packet_to_queue_from_side(r,2,p,n);}
''' + source[start:end] + r'''
int main(void){
 uint8_t wire[4]={1,2,3,4};rx_asynchronous(wire,4);
 assert(handoffs==1&&queued==1&&old_delivered==1&&!errors&&!locked);
 memset(wire,9,4);assert(saved[0]==1&&saved[3]==4);
 unsigned before_uart=uart_runs;
 rx_asynchronous(wire,4);assert(handoffs==2&&queued==2&&old_delivered==1&&uart_runs==before_uart);
 available=5572;uart_frees=1;rx_asynchronous(wire,4);
 assert(handoffs==3&&queued==3&&old_delivered==1&&!errors); /* Retired DMA alone suffices. */
 rx_asynchronous(NULL,4);rx_asynchronous(wire,0);assert(handoffs==3);
 // Captured later health report:49 wire bytes,2195 bytes of SDK work,
 //4096-byte reserve,6164 bytes free. Retiring DMA releases enough memory.
 uint8_t health[49]={0};available=6164;uart_frees=1;before_uart=uart_runs;
 rx_asynchronous(health,sizeof(health));
 assert(handoffs==4&&queued==4&&old_delivered==1&&!errors&&uart_runs==before_uart+1);
 // If DMA cannot release memory, dispatch old owned work before this frame.
 available=6164;uart_frees=0;rx_asynchronous(health,sizeof(health));
 assert(handoffs==5&&queued==5&&old_delivered==2&&!errors&&!locked);
 return 0;}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'reclaim'
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-Wno-unused-function','-fsanitize=address,undefined','-x','c','-','-o',str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)
