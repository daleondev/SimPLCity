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
