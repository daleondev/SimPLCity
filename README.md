# SimPLCity

![Banner](assets/banner.png)

## Software Dependencies

Ensure you have the following installed on your host machine:

* [CMake](https://cmake.org/download/) (v3.22+)
* [Ninja](https://ninja-build.org/)
* [GCC](https://gcc.gnu.org/) 15 or 16 for Linux simulation
* [Arm GNU Toolchain](https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads)
  15.2.Rel1 or 16.1 for STM32 firmware
* [OpenOCD](https://openocd.org/)

The runtime selects matching GCC 15.2 or GCC 16.1 libstdc++ integration
sources at configure time. Other compiler major versions are rejected so a
toolchain update cannot silently mix incompatible runtime internals.

## Per-thread stack sizes

Use the runtime factory to attach ThreadX creation attributes to an individual
standard thread without patching GCC's `std::thread` implementation:

```cpp
#include "runtime/thread.hpp"

auto logger = runtime::thread::create(
    {
        .priority = 17,
        .stack_size = 12U * 1024U,
    },
    run_logger,
    logger_argument);
```

`create_jthread` provides the corresponding `std::jthread` factory and
preserves its stop-token injection behavior. Both factories decay-copy the
callable and arguments before publishing the attributes, clear them if
construction fails, and restore any attributes previously published by the
calling thread.

`publish_attributes` remains available as a low-level escape hatch when an API
creates a standard thread internally. Publish immediately before calling that
API; the attributes apply once to its next standard-thread construction.

`stack_size` is measured in bytes; zero uses the configured
`RUNTIME_STD_THREAD_STACK_SIZE` default. `priority` follows ThreadX ordering,
where zero is the highest priority; `-1` uses the configured default. The
runtime rounds valid custom stack sizes up to its stack alignment and reports
invalid stack sizes or priorities with `std::errc::invalid_argument`.

## Persistent storage

The runtime exposes a read-only virtual root with two independent FileX
volumes:

* `/flash` is a 12 MiB FAT volume backed by LevelX on the external W25Q128 NOR
  flash. The remaining NOR capacity is reserved for LevelX reclamation and
  wear leveling. A completely blank chip is formatted once; an existing but
  invalid volume is never erased automatically.
* `/sd` is the existing FAT volume on the SDMMC card. Firmware never formats
  the card automatically.

Relative paths start at `/flash`. Renaming across the two volumes fails with
`EXDEV`, as it would across separate mounted filesystems. The old volatile RAM
disk is no longer part of the linker layout or runtime.

The SDMMC data and command inputs use explicit pull-ups. Card detect is PG2,
which is `D49` on **CN8 pin 14** of the NUCLEO-H753ZI; CN8 pin 8 is PC11/D46
and is already used by SDMMC D3. Card detect is advisory, so a missing DET wire
does not prevent mounting a readable card.

`RUNTIME_STORAGE_ERASE_FLASH_ON_BOOT=ON` is a destructive recovery option for
development only. Its default is `OFF`.

## Linux Simulation & Testing

To accelerate development and enable continuous integration (CI) without requiring the physical Nucleo board, this project can be compiled and run natively as a simulated Linux application. 

This is achieved by swapping the bare-metal ARM port of ThreadX for the ThreadX Linux/POSIX port, allowing your RTOS threads to run directly as a standard desktop process.

### Build Presets

The project now exposes explicit presets for both targets:

* `debug-stm32`
* `release-stm32`
* `debug-linux`
* `release-linux`
* `runtime-test-stm32`

### Build STM32 Firmware

```sh
cmake --preset debug-stm32
cmake --build --preset debug-stm32
```

The STM32 artifacts are generated in `build/debug-stm32/`.

To build the board-resident runtime conformance image:

```sh
cmake --preset runtime-test-stm32
cmake --build --preset runtime-test-stm32
```

The image reports `0x600D600D` in
`runtime_hardware_self_test_status` after all runtime phases pass. It can be
flashed and inspected with OpenOCD even when no serial terminal is available.
The storage phases exercise LevelX/FileX on `/flash` and a real write/read
round trip on `/sd`.

Run the complete configure, build, flash, test, and reporting flow with:

```sh
./scripts/run_hardware_self_test.sh
```

In VS Code, run the `Run Hardware Self-Test` task. The runner owns its OpenOCD
process, stops at the final result, prints the test phase and storage
diagnostics, and returns a nonzero exit status when the test fails or times
out. Stop an active debug session before starting it. The default 300-second
timeout can be changed with `RUNTIME_TEST_TIMEOUT_SECONDS`.

Use OpenOCD's `target/stm32h7x_dual_bank.cfg` target for this MCU. The test
image can exceed the first 1 MiB internal flash bank.

### Build Linux Simulation

```sh
cmake --preset debug-linux
cmake --build --preset debug-linux
```

The Linux executable is generated in `build/debug-linux/`.

### Run Linux Simulation

```sh
./build/debug-linux/Application
```

### VS Code Tasks

The workspace keeps the embedded debug flow intact and adds a Linux build task:

* `Build Debug` configures and builds `debug-stm32`
* `Flash` programs `build/debug-stm32/Application.elf`
* `Build Debug Linux` configures and builds `debug-linux`
