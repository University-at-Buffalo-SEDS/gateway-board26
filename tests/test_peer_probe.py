from pathlib import Path
import subprocess
import tempfile
import unittest

class PeerProbeTests(unittest.TestCase):
    def test_guarded_probe_lifetime_and_allocation_failure(self):
        s=(Path(__file__).resolve().parents[1]/'Core/Src/telemetry.c').read_text();a=s.index('void telemetry_hil_capture_requested_snapshot(void) {');b=s.index('\nstatic uint64_t node_now_since_ms',a);func=s[a:b]
        code=r'''
        #include <stdint.h>
        #include <stddef.h>
        #include <assert.h>
        #include <string.h>
        #define SEDS_IO -14
        #define TELEMETRY_USE_TLSF 1
        static uint32_t g_gateway_peer_probe_request, peer_probe_tick, tick, allocations, frees, exports;
        static int32_t g_gateway_peer_probe_result;
        static char *g_gateway_peer_probe_json;
        static char memory[1024];
        static int refuse, guard=1;
        static struct {void *r;} g_router={memory};
        static uint32_t HAL_GetTick(void){return tick;}
        static int telemetry_tlsf_admit(size_t n,size_t b){assert(n==8192&&b==2048);return guard;}
        static void *telemetry_can_tx_allocate(size_t n){assert(n==1024);allocations++;return refuse?NULL:memory;}
        static void telemetryFree(void *p){assert(p==NULL||p==memory);if(p)frees++;}
        static void telemetry_lock(void){}
        static void telemetry_unlock(void){}
        static int seds_router_export_client_stats(void *r,const char *peer,size_t len,char *p,size_t n){assert(r&&p==memory&&n==1024&&len==strlen(peer));exports++;strcpy(p,peer);return 0;}
        ''' +func+r'''
        int main(void){
         telemetry_hil_capture_requested_snapshot();assert(allocations==0);
         guard=0;g_gateway_peer_probe_request=1;telemetry_hil_capture_requested_snapshot();assert(allocations==0&&exports==0&&g_gateway_peer_probe_result==-14);
         guard=1;refuse=1;g_gateway_peer_probe_request=1;telemetry_hil_capture_requested_snapshot();assert(g_gateway_peer_probe_result==-14 && exports==0);
         refuse=0;g_gateway_peer_probe_request=1;telemetry_hil_capture_requested_snapshot();assert(!strcmp(memory,"GS") && g_gateway_peer_probe_result==0);
         g_gateway_peer_probe_request=2;telemetry_hil_capture_requested_snapshot();assert(!strcmp(memory,"AB") && allocations==2);
         tick=9999;telemetry_hil_capture_requested_snapshot();assert(frees==0);
         tick=10000;telemetry_hil_capture_requested_snapshot();assert(frees==1 && g_gateway_peer_probe_json==NULL);
         tick=UINT32_MAX-100;g_gateway_peer_probe_request=1;telemetry_hil_capture_requested_snapshot();tick=9900;telemetry_hil_capture_requested_snapshot();assert(frees==2);
         g_gateway_peer_probe_request=3;telemetry_hil_capture_requested_snapshot();assert(!strcmp(memory,"VB"));
         g_gateway_peer_probe_request=4;telemetry_hil_capture_requested_snapshot();assert(!strcmp(memory,"DAQ"));
         g_gateway_peer_probe_request=5;telemetry_hil_capture_requested_snapshot();assert(frees==3 && !g_gateway_peer_probe_json && !g_gateway_peer_probe_request);
        }
        '''
        with tempfile.TemporaryDirectory() as td:
         exe=str(Path(td)/'probe');subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c','-','-o',exe],input=code,text=True,check=True);subprocess.run([exe],check=True)
