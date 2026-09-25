from pathlib import Path
import subprocess,tempfile,unittest
ROOT=Path(__file__).resolve().parents[1]
class AllocatorTests(unittest.TestCase):
 def test_schema_reservation_and_nonblocking_fallback(self):
  s=(ROOT/'Core/Src/telemetry_hooks.c').read_text();a=s.index('void *telemetryMalloc(');b=s.index('\nvoid telemetryFree(',a)
  code=r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
typedef unsigned UINT;
typedef unsigned long ULONG;
typedef struct {int id;} TX_BYTE_POOL;
#define TX_NO_MEMORY 1
#define TX_SUCCESS 0
#define TX_NO_WAIT 0
#define TX_NULL NULL
static TX_BYTE_POOL normal={1}, emergency={2};
static TX_BYTE_POOL *rust_byte_pool_external=&normal,*rust_emergency_byte_pool_external=&emergency;
static unsigned g_telemetry_last_alloc_request,g_telemetry_max_alloc_request,g_telemetry_alloc_emergency_recoveries,g_telemetry_alloc_failure_request,g_telemetry_alloc_failure_available,g_telemetry_alloc_failure_fragments,g_telemetry_alloc_fail,g_telemetry_alloc_count;
static int calls[4],n,fail_first;
static UINT tx_byte_allocate(TX_BYTE_POOL*p,void**out,size_t size,unsigned wait){
 assert(wait==TX_NO_WAIT);assert(size>0);calls[n++]=p->id;
 if(fail_first&&n==1)return TX_NO_MEMORY;*out=(void*)p;return TX_SUCCESS;
}
static UINT tx_byte_pool_info_get(TX_BYTE_POOL*p,void*a,ULONG*b,ULONG*c,void*d,void*e,void*f){return 0;}
static void telemetry_memory_profile_sample(void){}
''' + s[a:b]+r'''
int main(void){
 assert(telemetryMalloc(3564)==&emergency); assert(n==1&&calls[0]==2);
 n=0; assert(telemetryMalloc(64)==&normal);assert(n==1&&calls[0]==1);
 n=0;fail_first=1;assert(telemetryMalloc(3564)==&normal);assert(n==2&&calls[0]==2&&calls[1]==1);
 n=0;assert(telemetryMalloc(64)==&emergency);assert(n==2&&calls[0]==1&&calls[1]==2);
}
'''
  with tempfile.TemporaryDirectory() as d:
   exe=str(Path(d)/'test');subprocess.run(['cc','-x','c','-','-o',exe],input=code,text=True,check=True);subprocess.run([exe],check=True)
if __name__=='__main__':unittest.main()
