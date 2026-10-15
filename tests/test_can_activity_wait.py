"""Exercise the actual ISR notification and bounded worker wait policy."""
from pathlib import Path
import subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class CanActivityWaitTests(unittest.TestCase):
    def test_lost_wakeup_burst_coalescing_and_sensor_fairness(self):
        source=(ROOT/'Core/Src/telemetry_thread.c').read_text()
        helper=source[source.index('static TX_SEMAPHORE telemetry_rx_activity;'):source.index('#define TELEMETRY_THREAD_STACK_SIZE')]
        code=r'''
#include <assert.h>
#include <stdint.h>
typedef struct {unsigned count;} TX_SEMAPHORE;
#define TX_SUCCESS 0
#define TX_NO_INSTANCE 1
static unsigned waits, sleeps, timeout, irq_during_wait;
static void telemetry_notify_rx_activity(void);
static int tx_semaphore_ceiling_put(TX_SEMAPHORE *s,unsigned ceiling){
 assert(ceiling==1);if(s->count<ceiling)s->count++;return TX_SUCCESS;
}
static int tx_semaphore_get(TX_SEMAPHORE*s,unsigned ticks){
 assert(ticks==1);waits++;
 if(irq_during_wait){irq_during_wait=0;telemetry_notify_rx_activity();}
 if(s->count){s->count--;return TX_SUCCESS;}
 timeout++;return TX_NO_INSTANCE;
}
static int tx_thread_sleep(unsigned ticks){assert(ticks==1);sleeps++;return TX_SUCCESS;}
'''+helper+r'''
int main(void){
 // CAN arriving between the empty check and wait must remain pending.
 telemetry_notify_rx_activity();telemetry_wait_for_activity();
 assert(waits==1&&!timeout&&!sleeps&&!telemetry_rx_activity.count);
 // Arbitrarily long interrupt bursts consume only one semaphore token.
 for(unsigned i=0;i<10000;i++)telemetry_notify_rx_activity();
 assert(telemetry_rx_activity.count==1);
 telemetry_wait_for_activity();assert(!timeout&&!telemetry_rx_activity.count);
 // An arrival while waiting wakes the worker, rather than being forgotten.
 irq_during_wait=1;telemetry_wait_for_activity();assert(!timeout);
 telemetry_notify_rx_activity();telemetry_wait_for_activity();
 assert(telemetry_immediate_wakes==4);
 telemetry_notify_rx_activity();telemetry_wait_for_activity();
 assert(sleeps==1&&telemetry_immediate_wakes==0&&telemetry_rx_activity.count==1);
 telemetry_wait_for_activity();assert(telemetry_rx_activity.count==0);
 // Sustained arrivals still grant the lower-priority sensor a scheduler tick.
 for(unsigned i=0;i<100;i++){telemetry_notify_rx_activity();telemetry_wait_for_activity();}
 assert(sleeps>=20&&telemetry_rx_activity.count<=1);
 telemetry_rx_activity.count=0;telemetry_immediate_wakes=0;
 telemetry_wait_for_activity();assert(timeout==1&&telemetry_immediate_wakes==0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/'activity'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c','-','-o',str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)
