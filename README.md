# Gateway Board 26 firmware

This STM32G491 gateway joins the fill-system CAN-FD network to its Pico-Fi
transport. The board-facing link is UART and preserves the Pico-Fi frame
boundary; conversion to the GroundStation-side I2C framing occurs in Pico-Fi
firmware. The gateway driver only transports complete SEDSNet packets. SEDSNet
discovery and learned subscriptions own routing, so application data is not
manually fanned out.

CMake fetches SEDSNet v4.0.19 and SEDS LaunchCore v1.0.0 without submodules.
LaunchCore derives linker scripts and the boot/OTA layout from
`Bootloader/board_config.h`.

## Build and validate

```sh
./build.py build --release
./build.py flash --release --method stm32prog-cli
./build.py clean
./build.py test
./build.py test --all --release
```

The normal flash workflow writes the complete `.factory.bin` at `0x08000000`.
Run `./build.py flash --help` for other host programmers. OTA packages use the
`.seds` extension and select delta or full-image recovery from the BSP layout.

`config/sedsnet.json` is the board-owned schema. `sim/board.json` declares the
STM32G491 memory limits and UART/CAN/peripheral model. The full test suite adds
release/OTA builds, long-duration memory probes, framed Pico-Fi traffic, fault
injection, and linked discovery, synchronization, and bidirectional command/ACK
validation through the GroundStation path.
