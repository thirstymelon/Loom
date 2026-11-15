# Loom v1.2.0 — Project Overview

Loom is a statically typed, block-oriented programming language with a C++ frontend and LLVM-based code generation. The repository contains the compiler, command-line driver, SDK modules, RP2040 target files, documentation website, editor extension, and test suites.

For native AArch64 bare-metal projects (`Bare-Metal = true`, `aarch64-none-elf`), `loom build` emits an ELF and flat binary only. Create a VM ISO separately with a bootloader and image tool; `examples/native-kernel/make-iso.sh` packages the ELF with Limine and `xorriso`. RP2040 remains a separate UF2 firmware target.

## Main areas

- `src/`: compiler frontend, semantic analysis, code generation, and command-line driver.
- `sdk/std/`: standard library modules.
- `sdk/targets/rp2040/`: RP2040 boot/startup sources, linker description, and board support package.
- `docs/`: documentation website, including the language guides and RP2040 guide.
- `tests/`: compiler, CLI, docs, language feature, validation, and SDK compile checks.
- `vscode-loom/`: Visual Studio Code extension.
- `datasheet/RP2040.pdf`: the RP2040 reference datasheet included with this project.

## RP2040 peripheral API direction

I²C and SPI initialization now return typed `I2C_Handle` and `SPI_Handle` values. They choose a controller from the physical pins and use deterministic beginner defaults (100 kHz I²C; 1 MHz, 8-bit, mode 0 SPI). Pass the handle to every operation or device-library initializer; this keeps each library tied to the bus it received. I²C, SPI, UART, and GPIO failures are captured in readable diagnostic globals and stop at the failing operation. The compiler permits intentionally discarded return values in statement position, so applications do not need to assign or check status-only results. SSD1306 and MPU6050 initializers accept an `I2C_Handle` and use their default device address. UART also returns a pin-selected handle and GPIO operational calls handle failures internally. ADC and other advanced BSP modules still expose legacy status-oriented APIs. ADC conversion waits are bounded, and peripheral FIFO/conversion polls use a 10 ms deadline after `RP2040_Init()`. Clock initialization assumes the Raspberry Pi Pico reference board's 12 MHz crystal.

## Validation status

The RP2040 peripheral suite compiles representative Loom programs against each module, and documentation checks compare each walkthrough and signature with the current BSP sources. These checks catch parser, type, import, documentation drift, and API regressions; they do not run on hardware or emulate MMIO. Before release, verify SPI clock/mode, I2C repeated-start/NACK/timeout handling, UART baud, and electrical timing on the intended RP2040 board and peripherals.


## Clear error boundaries

I²C/SPI/UART/GPIO and the current SSD1306/MPU6050 library operations handle failures internally. Diagnostic code/message globals remain available for inspection in a debugger. Returned values should represent data, such as an I²C register byte or an MPU6050 sample, rather than success flags. `Check` remains for older Boolean APIs while the remaining BSP modules are being converted.
