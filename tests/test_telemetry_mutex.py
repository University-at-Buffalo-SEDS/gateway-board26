"""Exercise the production hook pair, including handoff inside ThreadX put."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]

class TelemetryMutexTests(unittest.TestCase):
    def test_recursion_keeps_kernel_exclusion_inheritance_and_handoff(self):
        source = (ROOT/'Core/Src/telemetry_hooks.c').read_text()
        hooks = source[source.index('void telemetry_lock(void)'):source.index('void *telemetryMalloc')]
        stub = r'''#pragma once
#include <stddef.h>
typedef unsigned UINT;
typedef struct {unsigned id;} TX_THREAD;
typedef struct {unsigned id;} TX_MUTEX;
#define TX_SUCCESS 0U
#define TX_WAIT_FOREVER (~0U)
#define TX_NOT_OWNED 30U
#define TX_NULL NULL
UINT tx_mutex_get(TX_MUTEX *, unsigned);
UINT tx_mutex_put(TX_MUTEX *);
TX_THREAD *tx_thread_identify(void);
unsigned __get_IPSR(void);
'''
        program = r'''
#include "board_telemetry_mutex.h"
#include <assert.h>
static TX_MUTEX g_telemetry_mutex;
static BoardTelemetryMutex g_telemetry_owner;
static unsigned g_telemetry_mutex_ready=1, g_telemetry_lock_get_fail, g_telemetry_lock_put_fail;
static TX_THREAD a={1}, b={2}, *current=&a, *kernel_owner;
static unsigned gets, puts, isr, fail_get;
static void (*on_get)(void), (*on_put)(void);
TX_THREAD *tx_thread_identify(void) { return current; }
unsigned __get_IPSR(void) { return isr; }
UINT tx_mutex_get(TX_MUTEX *mutex,unsigned timeout) {
 assert(mutex==&g_telemetry_mutex && timeout==TX_WAIT_FOREVER && !isr);
 ++gets;
 if(fail_get) return 99;
 if(on_get) {void (*callback)(void)=on_get;on_get=NULL;callback();}
 assert(kernel_owner==NULL);kernel_owner=current;return TX_SUCCESS;
}
UINT tx_mutex_put(TX_MUTEX *mutex) {
 assert(mutex==&g_telemetry_mutex && kernel_owner==current && !isr);
 ++puts;kernel_owner=NULL;
 if(on_put) {void (*callback)(void)=on_put;on_put=NULL;callback();}
 return TX_SUCCESS;
}
'''+hooks+r'''
static void release_a_while_b_waits(void) { current=&a;telemetry_unlock();current=&b; }
static void handoff_to_b_before_put_returns(void) { current=&b;telemetry_lock();current=&a; }
int main(void) {
 for(unsigned round=0;round<10000;++round) {
  unsigned before_get=gets,before_put=puts;
  for(unsigned depth=0;depth<16;++depth) telemetry_lock();
  assert(gets==before_get+1 && g_telemetry_owner.depth==16 && kernel_owner==&a);
  for(unsigned depth=0;depth<15;++depth) telemetry_unlock();
  assert(puts==before_put && kernel_owner==&a);
  telemetry_unlock();assert(puts==before_put+1 && kernel_owner==NULL);
 }
 telemetry_lock();current=&b;on_get=release_a_while_b_waits;telemetry_lock();
 assert(kernel_owner==&b && atomic_load(&g_telemetry_owner.owner)==&b);
 telemetry_unlock();current=&a;
 telemetry_lock();on_put=handoff_to_b_before_put_returns;telemetry_unlock();
 assert(kernel_owner==&b && atomic_load(&g_telemetry_owner.owner)==&b && g_telemetry_owner.depth==1);
 current=&b;telemetry_unlock();current=&a;
 telemetry_lock();current=&b;unsigned before=puts;telemetry_unlock();
 assert(puts==before && kernel_owner==&a && g_telemetry_lock_put_fail==1);
 current=&a;telemetry_unlock();
 fail_get=1;telemetry_lock();assert(g_telemetry_lock_get_fail==1 && !kernel_owner && !atomic_load(&g_telemetry_owner.owner));fail_get=0;
 unsigned before_get=gets,before_put=puts;
 isr=1;telemetry_lock();telemetry_unlock();isr=0;
 current=NULL;telemetry_lock();telemetry_unlock();current=&a;
 g_telemetry_mutex_ready=0;telemetry_lock();telemetry_unlock();g_telemetry_mutex_ready=1;
 assert(gets==before_get && puts==before_put && !kernel_owner);
 telemetry_lock();telemetry_unlock();assert(!g_telemetry_owner.depth);
}
'''
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'tx_api.h').write_text(stub)
            binary=root/'mutex'
            subprocess.run(['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                '-I',str(root),'-I',str(ROOT/'Core/Inc'),'-x','c','-','-o',str(binary)],input=program,text=True,check=True)
            subprocess.run([str(binary)],check=True,timeout=10)
if __name__=='__main__':unittest.main()
