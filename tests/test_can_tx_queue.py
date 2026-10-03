from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CanTxQueueTests(unittest.TestCase):
    def test_owned_buffers_fragment_order_backpressure_and_recovery(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d)
            (p/'can_bus.h').write_text('''#pragma once
#include <stdint.h>
#include <stddef.h>
typedef int HAL_StatusTypeDef;
#define HAL_OK 0
#define HAL_ERROR 1
#define HAL_BUSY 2
HAL_StatusTypeDef can_bus_send_bytes(const uint8_t*,size_t,uint32_t);
''')
            (p/'can_tx_queue.h').write_text((ROOT/'Core/Inc/can_tx_queue.h').read_text())
            (p/'main.h').write_text('''#include <stdint.h>
extern uint32_t irq;
static inline uint32_t __get_PRIMASK(void){return irq;}
static inline void __disable_irq(void){irq=1;}
static inline void __set_PRIMASK(uint32_t n){irq=n;}
uint32_t HAL_GetTick(void);
''')
            (p/'test.c').write_text(r'''
#include "can_tx_queue.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
uint32_t irq;
static uint32_t tick;
static unsigned free_slots,allocations,frees,failed_alloc,hal_error,count;
static uint8_t wire[4096][64];static uint32_t ids[4096];
extern volatile uint32_t g_can_tx_queue_bytes,g_can_tx_pending,g_can_tx_rejected,g_can_tx_expired,g_can_tx_submitted;
uint32_t HAL_GetTick(void){return tick;}
void *telemetry_can_tx_allocate(size_t n){assert(!irq);if(failed_alloc)return NULL;allocations++;return malloc(n);}
void telemetryFree(void *p){assert(!irq);frees++;free(p);}
HAL_StatusTypeDef can_bus_send_bytes(const uint8_t *p,size_t n,uint32_t id){
 assert(irq && n==64);
 if(hal_error)return HAL_ERROR;
 if(!free_slots)return HAL_BUSY;
 free_slots--;assert(count<4096);memcpy(wire[count],p,64);ids[count++]=id;return HAL_OK;
}
static void interrupt(void){unsigned a=allocations,f=frees;irq=1;can_tx_queue_pump();assert(irq && allocations==a && frees==f);irq=0;}
static void drain(void){while(g_can_tx_pending){free_slots=3;interrupt();can_tx_queue_service();}}
int main(void){
 uint8_t data[128];for(unsigned i=0;i<128;i++)data[i]=i;
 assert(can_tx_queue_submit(NULL,1,0)==HAL_ERROR);
 assert(can_tx_queue_submit(data,0,0)==HAL_ERROR);
 assert(can_tx_queue_submit(data,129,0)==HAL_ERROR);
 assert(can_tx_queue_submit(data,128,0x104)==HAL_OK);
 assert(g_can_tx_pending==1 && count==0);memset(data,99,sizeof(data));
 free_slots=1;interrupt();assert(count==1 && frees==0 && g_can_tx_pending==1);
 hal_error=1;free_slots=3;interrupt();assert(count==1);
 hal_error=0;interrupt();assert(count==3 && g_can_tx_pending==0 && frees==0);
 for(unsigned f=0;f<3;f++){
  assert(wire[f][0]=='S' && wire[f][1]=='D' && wire[f][2]==4 && wire[f][3]==0);
  assert(wire[f][4]==f && wire[f][5]==3 && wire[f][7]==128 && wire[f][8]==0 && ids[f]==0x104);
  unsigned n=f==2?18:55;for(unsigned i=0;i<n;i++)assert(wire[f][9+i]==f*55+i);
 }
 assert(wire[0][6]==1 && wire[1][6]==0 && wire[2][6]==2);
 can_tx_queue_service();assert(frees==1 && !g_can_tx_queue_bytes);
 // Hard byte/count limit: repeated refusal cannot leak an allocation.
 free_slots=0;unsigned accepted=0;
 while(can_tx_queue_submit(data,128,0x104)==HAL_OK)accepted++;
 assert(accepted>1 && accepted<=48 && g_can_tx_queue_bytes<=6144);
 unsigned a=allocations,pending=g_can_tx_pending;
 for(unsigned i=0;i<100;i++)assert(can_tx_queue_submit(data,128,0x104)==HAL_BUSY);
 assert(allocations==a && g_can_tx_pending==pending);
 drain();assert(!g_can_tx_queue_bytes && allocations==frees);
 // Ordered messages keep each packet's fragments consecutive.
 for(unsigned i=4;i<count;i++){
  if(wire[i][4])assert(wire[i][3]==wire[i-1][3] && wire[i][4]==wire[i-1][4]+1);
 }
 failed_alloc=1;free_slots=0;assert(can_tx_queue_submit(data,1,0)==HAL_BUSY);failed_alloc=0;
 // Expire a partly emitted packet across timer wrap; no stale remainder sent.
 tick=UINT32_MAX-500;free_slots=1;assert(can_tx_queue_submit(data,128,0)==HAL_OK);
 unsigned before=count;tick=499;free_slots=3;interrupt();assert(count==before && g_can_tx_expired==1);
 can_tx_queue_service();assert(!g_can_tx_queue_bytes && allocations==frees);
 free_slots=0;assert(can_tx_queue_submit(data,128,0)==HAL_OK);can_tx_queue_reset();
 assert(!g_can_tx_queue_bytes && !g_can_tx_pending && allocations==frees);
 // Repeated small packets exercise sequence and queue reuse/wrap.
 for(unsigned i=0;i<300;i++){free_slots=3;assert(can_tx_queue_submit(data,1,0)==HAL_OK);}
 assert(allocations==frees);
}
''')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                            '-I',str(p),str(ROOT/'Core/Src/can_tx_queue.c'),str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

    def test_queue_allocation_preserves_control_reserve(self):
        source = (ROOT/'Core/Src/telemetry_hooks.c').read_text()
        start = source.index('void *telemetry_can_tx_allocate(')
        end = source.index('void telemetryFree(', start)
        harness = r'''
#include <stddef.h>
#include <assert.h>
typedef unsigned long ULONG;
typedef struct {ULONG available;} TX_BYTE_POOL;
#define TX_NULL NULL
#define TX_SUCCESS 0
static TX_BYTE_POOL small={4200},large={0};
static TX_BYTE_POOL *rust_byte_pool_external=&small,*rust_emergency_byte_pool_external=&large;
static unsigned allocations,denied;
static int tx_byte_pool_info_get(TX_BYTE_POOL *p,void*a,ULONG*v,void*b,void*c,void*d,void*e){
 (void)a;(void)b;(void)c;(void)d;(void)e;*v=p->available;return 0;
}
static int telemetry_tlsf_admit(size_t additional,size_t largest){assert(additional==largest+512);return !denied;}
static void *telemetryMalloc(size_t n){(void)n;allocations++;return &small;}
''' + source[start:end] + r'''
int main(void){
#ifdef TELEMETRY_USE_TLSF
 assert(telemetry_can_tx_allocate(128));denied=1;
 assert(!telemetry_can_tx_allocate(128));assert(allocations==1);
#else
 assert(!telemetry_can_tx_allocate(128));assert(!allocations);
 large.available=24;assert(telemetry_can_tx_allocate(128));assert(allocations==1);
 small.available=1;large.available=0;assert(!telemetry_can_tx_allocate(128));
 rust_byte_pool_external=NULL;assert(!telemetry_can_tx_allocate(128));
#endif
}
'''
        with tempfile.TemporaryDirectory() as d:
            p=Path(d);(p/'test.c').write_text(harness)
            for defines in [[],['-DTELEMETRY_USE_TLSF']]:
                subprocess.run(['cc','-std=c11',*defines,str(p/'test.c'),'-o',str(p/'test')],check=True)
                subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':unittest.main()
