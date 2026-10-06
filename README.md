# Gateway Board 26 firmware

This STM32G491 gateway joins the fill-system CAN-FD network to its Pico-Fi
transport. The board-facing link is UART and preserves the Pico-Fi frame
boundary; conversion to the GroundStation-side I2C framing occurs in Pico-Fi
firmware. The gateway driver only transports complete SEDSNet packets. SEDSNet
discovery and learned subscriptions own routing, so application data is not
manually fanned out.

CMake tracks SEDSNet `main`, using the existing on-disk source when offline,
and fetches SEDS LaunchCore v1.0.0 without submodules. See
[SEDSnet source selection](docs/sedsnet-source.md).
LaunchCore derives linker scripts and the boot/OTA layout from
`Bootloader/board_config.h`.

## Build and validate

```sh
./build.py build --release
./build.py flash --release --method stm32prog-cli
./build.py clean
./build.py test
./build.py test --all --release
./build.py test --all --release --ultra-soak
```

On Docker hosts that cannot create bridge interfaces (including the Jupiter
validation host), prefix the command with
`SEDS_FIRMWARE_SIM_DOCKER_NETWORK=host`. The linked test requires GroundStation
to label all seven graph nodes, attribute real payload traffic to each board,
and correlate a routed valve command with its returned state ACK.

`--ultra-soak` keeps the normal 16-second full-network test first, then adds a
separate 600,000 ms firmware-time fault/rejoin, command/ACK, and memory-leak
qualification. Commands must execute and return an ACK throughout the soak,
including its final interval.

The normal flash workflow writes the complete `.factory.bin` at `0x08000000`.
Run `./build.py flash --help` for other host programmers. OTA packages use the
`.seds` extension and select delta or full-image recovery from the BSP layout.

`config/sedsnet.json` is the board-owned schema. `sim/board.json` declares the
STM32G491 memory limits and UART/CAN/peripheral model. The full test suite adds
release/OTA builds, long-duration memory probes, framed Pico-Fi traffic, fault
injection, and linked discovery, synchronization, and bidirectional command/ACK
validation through the GroundStation path.


## Regenerating with STM32CubeMX

Open the checked-in `.ioc` file and generate with the CMake toolchain. Keep user
code enabled. The `.ioc` is the source of truth for the ThreadX and USBX pool
sizes; unit tests compare those values with the generated Azure RTOS headers so
regeneration cannot silently shrink, grow, or repartition the pools.
The application pool is 74 KiB (75,776 bytes), including additional headroom
for full-network discovery bursts; the separate large-allocation emergency
pool remains 16 KiB. Qualification retains the 1 KiB minimum normal-pool
reserve rather than accepting near-exhaustion as a passing result.

The top-level CMake project is board-owned and reconnects generated STM32
sources with SEDSNet, LaunchCore, its generated linker scripts, persistence, and
the simulator probes. After generation, run
`python3 build.py test --full --release` before flashing or committing.

## Hardware watchdog

[Board watchdog configuration and validation](docs/watchdog.md). Build with
`./build.py build --release --watchdog`.
Watchdogs are opt-in and require the matching bootloader.
## Memory pressure and forwarding

The telemetry loop drains TX before and between short CAN receive slices. Discovery
and time-sync maintenance continue regularly. SEDSNet 4.0.35 avoids duplicate route
and schema buffers and can reject new work before transient allocations consume
space needed for dispatch.

Build the allocator experiment with `./build.py build --release --allocator tlsf`.
ThreadX still schedules the threads. In TLSF builds, the registered admission probe
checks free bytes and, for larger requests, the largest contiguous free block. It
reserves 4 KiB for normal work and a smaller 512-byte reserve for ACK processing.
`g_gateway_memory_admission_drops` counts refused operations; it is distinct from
`g_rx_dropped_frames` (CAN ring overruns) and the allocator failure/panic counters.
Refusals do not create allocated error-log packets. Default ThreadX allocator
builds do not register this TLSF-specific probe.

The allocator pool sizes are unchanged. Fixed queue preallocation is 1 KiB per
queue, while the shared retained-memory ceiling remains 16 KiB. The reduced
preallocation releases scratch headroom; overflow remains bounded.

The tested gateway/Pico UART pair runs at 1,000,000 baud (`Pico1M` preset); both
ends must match. This is separate from the 115200-baud radio link. Hardware tests
show reduced CAN loss, not lossless service or indefinite OOM immunity. Soak tests
must monitor forwarding, all board liveness, pressure refusals, allocator failures,
and ring overruns together.

## Experimental SEDSnet development builds

Use `python3 build.py build --release --sedsnet-ref dev` to build the current
SEDSnet `dev` commit. The same option is accepted by `test` and `flash`.
Normal builds continue to select `main`. Each branch has its own source cache;
when the network is unavailable the last usable on-disk source is retained.
The selected revision is printed during configure. An explicit CMake source
override remains local and is never fetched or reset.

The compact packet arena is initialized before router startup with a 2 KiB
payload budget and 16 handles, plus fixed startup metadata.
The previous 8 KiB/64-handle arena reserved 10 KiB while empty and could
starve discovery on real hardware. SEDSnet 4.1.3 retains existing heap
payloads when optional arena parking cannot fit them; queue and allocator
admission bounds still apply. Use `--packet-store heap` with TLSF for the
current recovery configuration; the smaller arena remains experimental.
It is separate from TLSF, which replaces only board-owned telemetry allocation
hooks; ThreadX scheduling continues unchanged. The arena does not compact
arbitrary application allocations, and queue parking remains uncompressed.

```sh
python3 build.py test --release --allocator tlsf --packet-store compact
python3 build.py flash --release --allocator tlsf --packet-store compact \
  --pico-baud 1000000 --method stm32prog-cli
```

`--packet-store compact` selects `dev` automatically. Both Pico UART ends must
use the same baud; 1 Mbaud is the paired Pico build, independent of the 115200
radio link. The board-owned watchdog is enabled by default; use its
matching factory bootloader. `--packet-store heap` disables the arena.
The new arena API must be present in an offline fallback; an older source is
rejected clearly. This candidate is for testing; linked ten-minute qualification
and a throughput claim remain pending.
