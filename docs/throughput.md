# Gateway throughput measurement

The gateway forwards fill-system CAN-FD traffic to GroundStation over a
1 Mbaud UART to the Pico-Fi. Pico-Fi then forwards it over Ethernet.

There are two measurement-only builds. Neither ships.

| Environment variable            | CMake option                 | Effect |
|---------------------------------|------------------------------|--------|
| `GATEWAY_CYCLE_PROFILE=1`       | `GATEWAY_CYCLE_PROFILE`      | DWT cycle accounting per telemetry-loop phase and ISR (`Core/Inc/gateway_cycle_profile.h`). |
| `GATEWAY_CYCLE_PROFILE_NESTED=1`| `GATEWAY_CYCLE_PROFILE_NESTED` | Also times SEDSnet ingress, UART/CAN egress copies, TLSF malloc/free and admission. It uses `-finstrument-functions`, which inflates costs, so compare it only with builds of the same kind. |
| `GATEWAY_LOADGEN_FPS=<n>`       | `GATEWAY_LOADGEN_FPS`        | Synthetic CAN ingress at *n* frames/s (`Core/Inc/gateway_loadgen.h`). |

Both builds take 512 B from the SEDSnet emergency pool (16,384 to 15,872 B)
to hold their state. The simulator image has fewer than 100 B of SRAM spare.
Load-generator images do not publish the gateway's own reliable battery
telemetry. Without a GroundStation those packets are never acknowledged and
would keep the router's transmit queue busy. The GS-unreachable scenario
keeps them on purpose (`GATEWAY_LOADGEN_OWN_TELEMETRY=1`).

Always build measurement images in their own directory, so they cannot be
mistaken for a flashable release:

```sh
GATEWAY_CYCLE_PROFILE=1 python3 build.py --build-subdir Profile_Script build --release
```

## Simulator scenario

```sh
python3 sim/throughput.py --gates                # every gate below (test --all stage)
python3 sim/throughput.py --fps 1500             # one rate, judged against its gate
python3 sim/throughput.py --sweep 600,1000,1500  # report only
python3 sim/throughput.py --fps 600 --gs-unreachable
```

The scenario builds `build/Throughput_Script` with the simulator probes, the
cycle profile and the load generator. It then runs the gateway alone in
Renode for 10 s of virtual time.

The generator starts 3 s after boot. It injects canonical, CRC-valid KG1000
packets addressed to GroundStation through the CAN receive ring, in bursts of
fps/1000 frames every 1 ms tick. Each window starts after a 1 s ramp.

| Gate | Offered | Pass condition |
|------|---------|----------------|
| Sustained | 600/s | At least 99% of injected frames leave on the UART, no ring drop, no allocation failure. |
| Overload | 1,500/s and 3,000/s | At least 90% of the 1,200/s capacity is forwarded, at least 99% of the loss is ISR ring drops, no allocation failure. |
| GS unreachable | 600/s, own telemetry kept | Same as sustained. |
| GS unreachable, idle | 1/s, own telemetry kept | Relative CPU busy at most 25%. |

The result is deterministic in virtual time, so host load does not change it.
Each run takes about two minutes of wall time.

**Target: 600 frames/s.** This is twice the fill bus's offered load of about
291 frames/s, rounded up. That load is:

| Board    | Frames/s |
|----------|----------|
| DAQ      | 100.6    |
| Valve    | 56.2 telemetry plus about 125 SD batches to DAQ |
| Actuator | 9.2      |
| Control  | about 3  |

The gateway receives and decodes every frame on the bus, but forwards only
about 166/s today. Every synthetic frame is forwarded, so the target is also
3.6 times today's forwarded rate.

### Limits

- **Renode is not cycle accurate.** It executes 100 M instructions per
  virtual second, while DWT CYCCNT counts at 170 MHz. One profiled "cycle"
  is therefore 1/1.7 of an instruction. Treat rates, saturation points and
  profile shares as relative evidence: compare them only with other Renode
  runs.
- A Cortex-M4 at 170 MHz with flash wait states retires roughly 120-150 M
  instructions/s. Real limits should be about 1.2-1.5 times the Renode
  figures, but only the bench procedure below measures them.
- The UART model times every character at the configured baud, so the
  1 Mbaud link limit is modelled.
- The FDCAN peripheral, `HAL_FDCAN_GetRxMessage` and CAN bus arbitration are
  bypassed. Their per-frame ISR cost is in the `can_isr` slot of a real-bus
  profile.
- There is no GroundStation, so the router forwards by its single-route
  fallback and not through discovered routes. Routing in a linked network
  costs somewhat more.
- Synthetic frames are canonical packets. Boards send compact SDT frames, and
  those take a template lookup instead.

## Results (Renode-relative, 2026-11-11)

All results are from single-board runs. Host load was 12-21. Virtual time
is deterministic, so host load does not change them.

### Saturation and overload

| Offered (frames/s) | Before: forwarded | Before: busy | After: forwarded | After: busy |
|---|---|---|---|---|
| 600   | 600, lossless | 51% | 600, lossless | 51% |
| 1,000 | lossless | 81% | lossless | n/a |
| 1,100 | lossless | 87% | lossless | n/a |
| 1,200 | 458 (collapse) | 102% | 1,200, lossless | 94% |
| 1,400 | n/a | n/a | 1,224 | 97% |
| 1,500 | 434 | 105% | 1,223 | 97% |
| 2,000 | 384 | 110% | n/a | n/a |
| 3,000 | 348 | 119% | 1,218 | 98% |

Before the fixes, 11% of the overload loss happened inside the router. After
them, all of it is shed in the ISR ring. Busy above 100% means preempting
ISRs are counted inside thread spans.

The CPU saturates first. The link budgets are:

- **UART:** 1 Mbaud is about 100 kB/s.
- **CAN:** 3.54 Mbit/s nominal, no BRS.

Both are far above 1,200 small frames/s. The UART transmit queue's high-water
mark stayed at 2. Neither the allocator nor the queue depth was limiting.

### Where the cycles go

These figures are for the unpaced firmware at 1,000 frames/s.

| Path | Share of CPU | Cost per frame (relative cycles) |
|------|--------------|----------------------------------|
| SEDSnet queue dispatch: routing, UART-side encode, egress callback | 65.4% | ~104K (about 61K instructions) |
| SEDSnet CAN-side decode and enqueue | 11.9% | 20.2K |
| Idle loop and queue polling | ~5% | n/a |
| TLSF allocator (5.1 malloc + 5.1 free per frame) | 2.1% | 433 + 251 per call |
| CAN ring pop and reassembly | 1.5% | 2.6K |
| UART enqueue, DMA and ISR | 0.6% | 528 + ISR |
| Memory admission (2 per frame) | 0.5% | 449 per call |
| Loop maintenance: discovery, time sync, OTA | 0.6% | n/a |

Above saturation, before pacing, SEDSnet ingress rose to 146-180K cycles per
frame. Every handoff into a full router queue paid for victim selection. The
full-ring dominant-sender eviction cost 10-33K cycles per millisecond in the
ISR.

### GroundStation unreachable

| Offered | Before | After |
|---------|--------|-------|
| Idle    | 61% busy | 15% busy |
| 300/s   | lossless, 83% busy | lossless, 63% busy |
| 600/s   | lossless, 94% busy | lossless, 92% busy |
| 1,000/s | n/a | 229/s forwarded; 58% of the loss is inside the router |

The router re-pops its own unroutable reliable packets with no backoff, so
every queue call burns its whole 1 ms budget. The gateway now backs off
transmit-only service in that state.

Today's fill-bus load of 291 frames/s is still decoded and forwarded without
loss. The capacity, however, drops from 1,200/s to between 600 and 1,000/s.
This scenario has no CAN peers either, so it is harsher than losing only the
Pico link: in a linked network DAQ would still take the SD-card copy.

### Ruled out

Building SEDSnet at opt-level `s` needs 90 KB more flash, and at opt-level `2`
it needs 314 KB more. The image has 29 KB free. SEDSnet stays at `z`.

## SEDSnet proposal (not changed here)

1. **Backoff for unroutable queued packets.**
   - The problem: when a queued transmit item has no eligible side, it goes
     back in the queue with `retry_after_ms = now`. `drain_queues_with_timeout`
     then loops until its deadline.
   - The measured cost: a single board with no route spends 61% of its CPU
     idle-spinning. With GS unreachable, 16 such items occupy the transmit
     queue, and forwarding at 1,000/s offered collapses to 229/s.
   - The proposal: an exponential `retry_after_ms` for items with no
     eligible side, and a bound on how many unroutable items the queue
     retains.
2. **Per-packet dispatch cost.**
   - The problem: forwarding one small packet costs about 61K instructions
     to dispatch and about 12K to decode (Renode). That cost sets the
     gateway's capacity: 1,200 frames/s in Renode, roughly 1,400-1,800/s
     estimated for hardware.
   - The proposal: profile `remote_side_plan_with_selection`,
     `encode_side_transport_frames` and the relay `enqueue_to_sides` path,
     using the gateway's `GATEWAY_CYCLE_PROFILE_NESTED` build on a bench,
     or a host benchmark.
3. **Saturated receive-queue admission.**
   - The problem: once the shared queue budget is full, each admission
     scans for a victim, which costs 7-9 times a normal handoff.
   - The status: the gateway now paces itself, but other relays would
     benefit.

## Bench procedure (real cycles)

This procedure needs an ST-Link and the normal flash procedure. The steps are
for whoever runs the bench (Rylan or the Mac agent).

1. Build a profile image. For a run with the load generator (step 4 B), add
   `GATEWAY_LOADGEN_FPS=600`.

   ```sh
   GATEWAY_CYCLE_PROFILE=1 python3 build.py --build-subdir Profile_Script build --release
   ```

2. Flash `build/Profile_Script/gateway_board.factory.bin` the usual way.
3. Bring up the full fill bench: GB, AB, VB, DAQ, Pico-Fi and GroundStation.
   Wait 60 s for discovery and time sync.
4. Pick one of two runs:
   - **A. Real traffic.** This gives real per-path cycles at today's load.
   - **B. Load generator.** Lower the rate before step 5 with
     `set var g_gateway_loadgen_fps = 0`. Then, at step 5, set it to the rate
     under test (600, 1000, 1500, ...). Ramp it until
     `g_rx_dropped_frames` or `g_gateway_uart_tx_queue_drops` start to rise.
     That rate is the hardware saturation point.
5. With `arm-none-eabi-gdb build/Profile_Script/gateway_board.elf` attached,
   without halting for long:

   ```gdb
   set var g_gateway_cycle_profile_reset = 1
   continue          # let it run 10 s, then Ctrl-C
   p/u g_gateway_cycle_profile_since
   p/u g_gateway_cycle_profile_wall
   p/u g_gateway_cycle_profile
   p/u g_gateway_uart_tx_frames
   p/u g_fdcan_rx_count
   p/u g_rx_dropped_frames
   ```

6. Compute:
   - **Elapsed cycles** = `wall - since`, modulo 2^32. A window up to 25 s
     cannot wrap.
   - **Each slot's share** = `total / elapsed`.
   - **Cycles per call** = `total / count`.
   - **CPU busy** = the sum of slots 0-5, 7, 8 and 16. Slot 6 is the
     telemetry thread's wait and is not CPU work.
   - **Frames/s** = the deltas of `g_fdcan_rx_count` and
     `g_gateway_uart_tx_frames` over the window. Take both readings before
     and after.
7. Repeat with `GATEWAY_CYCLE_PROFILE_NESTED=1` to split `can_drain` and
   `queues` into SEDSnet ingress, egress copies, allocator and admission. The
   hooks add overhead, so use these numbers for attribution only.
