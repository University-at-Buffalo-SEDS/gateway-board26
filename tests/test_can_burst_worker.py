"""Run the actual worker against transient and permanent CAN bursts."""
from pathlib import Path
import subprocess, tempfile, unittest

class BurstWorkerTests(unittest.TestCase):
    def test_burst_drain_keeps_uart_and_network_service_bounded(self):
        source = (Path(__file__).resolve().parents[1]/'Core/Src/telemetry_thread.c').read_text()
        constants = source[source.index('#define TELEMETRY_QUEUE_SERVICE_BUDGET_MS'):source.index('volatile uint32_t g_telemetry_stack_remaining')]
        worker = source[source.index('void telemetry_thread_entry('):source.index('UINT create_telemetry_thread(')]
        code = r'''
#include <stdint.h>
#include <assert.h>
#include <setjmp.h>
typedef unsigned long ULONG;
#define BOARD_WATCHDOG_NETWORK 1
#define SERVICE(phase, call) do { call; } while(0)
static unsigned pending, ticks, network_calls, first_network_pending;
static unsigned uart_calls, cycles, frames, continuous, network_pending;
static unsigned inject_maintenance_burst, needs_maintenance_drain, maintenance_frames;
static unsigned handoff_drops;
static uint32_t g_gateway_telemetry_loop_count;
static int huart2, hfdcan2;
static jmp_buf completed;
static uint32_t HAL_GetTick(void) { return ticks; }
static int telemetry_uart_init(void *p) { (void)p; return 0; }
static int init_telemetry_router(void) { return 0; }
static void can_bus_init(void *p) { (void)p; }
static void telemetry_notify_rx_activity(void) {}
static void can_bus_set_rx_wakeup_callback(void (*p)(void)) { assert(p); }
static void telemetry_wait_for_activity(void);
static int can_bus_health_ok(void) { return 1; }
static void board_watchdog_progress(unsigned n) { (void)n; }
static void telemetry_uart_process(void) { uart_calls++; }
static int dispatch_tx_queue_timeout(unsigned n) {
    (void)n;
    // Encoding/dispatching a catalog must not delay an existing FIFO burst.
    assert(pending < 16);
    ticks += 6;
    return 0;
}
static unsigned can_bus_rx_pending(void) { return pending; }
static unsigned can_bus_process_rx_for(unsigned count, unsigned ms) {
    unsigned n = count < pending ? count : pending;
    pending -= n; frames += n;
    // A bounded handoff queue holds sixteen packets. The old
    // backlog-dependent dispatch waits for four slices and loses packets.
    const unsigned room = 16 - network_pending;
    const unsigned accepted = n < room ? n : room;
    network_pending += accepted; handoff_drops += n - accepted; ticks += ms;
    if (continuous) pending += n;
    return n;
}
static int process_all_queues_timeout(unsigned n) {
    (void)n;
    if (needs_maintenance_drain) {
        assert(frames >= maintenance_frames + 8); // One bounded CAN slice precedes further dispatch.
        needs_maintenance_drain = 0;
    }
    if (!network_calls) first_network_pending = pending;
    network_calls++;
    // Bounded asynchronous RX dispatch need not drain an entire CAN slice.
    unsigned processed = network_pending < 16 ? network_pending : 16;
    network_pending -= processed;
    // A schema decode can take longer than its requested service budget.
    ticks += 8;
    return 0;
}
static int telemetry_poll_discovery(void) {
    if (inject_maintenance_burst) {
        // Observed schema maintenance takes 9 ms while CAN IRQs keep arriving.
        ticks += 9; pending += 32; needs_maintenance_drain = 1; maintenance_frames = frames;
        inject_maintenance_burst = 0;
    }
    return 0;
}
static int telemetry_poll_timesync(void) { return 0; }
static void ota_stream_poll(void) {}
static void telemetry_hil_capture_requested_snapshot(void) {}
static void sample_telemetry_stack(void) {}
static void tx_thread_sleep(unsigned n) {
    assert(n == 1);
    // A transient handoff must be dispatched before yielding, even when
    // some CAN frames remain for the next bounded worker cycle.
    if (!continuous) assert(network_pending == 0);
    assert(network_calls >= cycles + 1);
    assert(uart_calls >= cycles + 2);
    ticks += n;
    if (++cycles == 5) longjmp(completed, 1);
}
''' + constants + worker + r'''
static void reset(unsigned forever) {
    pending=40; ticks=0; network_calls=0; first_network_pending=99;
    uart_calls=cycles=frames=network_pending=0; continuous=forever;
    inject_maintenance_burst=needs_maintenance_drain=maintenance_frames=handoff_drops=0;
}
static void telemetry_wait_for_activity(void){tx_thread_sleep(1);}
int main(void) {
    reset(0);
    if (!setjmp(completed)) telemetry_thread_entry(0);
    // Dispatch after one handoff batch even while CAN still has a backlog.
    // Do not relax queue capacity or extend the worker's wall-time budget.
    assert(handoff_drops == 0);
    assert(first_network_pending == 24);
    assert(pending == 0 && frames == 40);
    reset(0); pending=0; inject_maintenance_burst=1;
    if (!setjmp(completed)) telemetry_thread_entry(0);
    assert(frames == 32 && pending == 0 && network_pending == 0 && handoff_drops == 0);
    reset(1);
    if (!setjmp(completed)) telemetry_thread_entry(0);
    // A continuously replenished ring services each handoff once and yields;
    // do not repeat discovery maintenance after the bounded combined pass.
    assert(network_calls == 5 && frames == 80 && handoff_drops == 0 && network_pending == 0);
    assert(first_network_pending == 40);
    ticks=UINT32_MAX-3; network_calls=cycles=frames=0;
    if (!setjmp(completed)) telemetry_thread_entry(0);
    assert(network_calls == 5 && frames == 80 && handoff_drops == 0 && network_pending == 0);
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp)/'worker'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-x','c','-','-o',str(exe)],input=code,text=True,check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__': unittest.main()
