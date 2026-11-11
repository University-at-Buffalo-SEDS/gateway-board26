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
would keep the router's transmit queue busy.

Always build measurement images in their own directory, so they cannot be
mistaken for a flashable release:

```sh
GATEWAY_CYCLE_PROFILE=1 python3 build.py --build-subdir Profile_Script build --release
```

## Simulator scenario

```sh
python3 sim/throughput.py                       # pass/fail at 600 frames/s
python3 sim/throughput.py --sweep 600,1000,1500 # saturation search
```

The scenario builds `build/Throughput_Script` with the simulator probes, the
cycle profile and the load generator. It then runs the gateway alone in
Renode for 10 s of virtual time.

The generator starts 3 s after boot. It injects canonical, CRC-valid KG1000
packets addressed to GroundStation through the CAN receive ring, in bursts of
fps/1000 frames every 1 ms tick. The scenario fails if, after a 1 s ramp,
any of these is true:

- fewer than 99% of the injected frames leave on the UART;
- the CAN ring drops a frame;
- an allocation fails.

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
