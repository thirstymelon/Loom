<div align="center">

<img src="logo/Loom_Logo.png" alt="Loom Language Logo" width="140" height="140" />

# Loom Programming Language

**A modern, deterministic, high-performance systems programming language with an LLVM backend and first-class bare-metal support.**

[![Version](https://img.shields.io/badge/version-1.2.0-92a9e1.svg)](https://github.com/thirstymelon/Loom/releases)
[![Backend](https://img.shields.io/badge/backend-LLVM%20Native-blue.svg)](https://llvm.org/)
[![Embedded](https://img.shields.io/badge/target-RP2040%20Bare--Metal-brightgreen.svg)](https://thirstymelon.github.io/Loom/embedded-rp2040.html)
[![VS Code](https://img.shields.io/badge/vscode-extension%20in%20repo-informational.svg)](vscode-loom/)
[![macOS PKG](https://img.shields.io/badge/macos-pkg%20in%20releases-lightgrey.svg)](https://github.com/thirstymelon/Loom/releases)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

[Documentation Hub](https://thirstymelon.github.io/Loom/) • [Getting Started](https://thirstymelon.github.io/Loom/getting-started.html) • [Beginner's Guide](https://thirstymelon.github.io/Loom/tutorials/beginner-guide.html) • [RP2040 Bare-Metal](https://thirstymelon.github.io/Loom/embedded-rp2040.html) • [Language Reference](https://thirstymelon.github.io/Loom/reference.html)

</div>

---

## Introduction

Loom v1.2.0 is a block-oriented, statically typed systems programming language with explicit low-level types, readable block syntax, an LLVM-based compiler, and an RP2040 target SDK.

The C++ frontend parses and checks Loom programs and the LLVM backend emits target code. The project includes hosted/native tooling and an RP2040 target. RP2040 I²C and SPI return typed handles from pin-based initialization; device libraries receive the handle they should use. These calls capture failures internally and retain readable diagnostics. Compile checks do not execute MMIO or substitute for board-level validation; review the [RP2040 BSP handbook](docs/rp2040-reference.html#mod-i2c) for setup and hardware guidance.

### Native AArch64 bare-metal images

With `Bare-Metal = true` and an AArch64 ELF target such as `aarch64-none-elf`, `loom build` emits a kernel ELF and flat `.bin`; it does not package an ISO. The flat binary has no entry-point or segment metadata, so use the ELF with a compatible bootloader when creating a bootable VM image. The [native-kernel ISO helper](examples/native-kernel/make-iso.sh) packages the ELF with [Limine](https://github.com/limine-bootloader/limine) and `xorriso`. Attach the resulting ISO to an AArch64 VM with UEFI firmware. RP2040 bare-metal builds remain UF2-based.

---

## Core Capabilities

- **LLVM Backend**: Generates code for configured targets and applies optimization passes.
- **Deterministic Memory Model**: Free of garbage collection pauses and hidden runtime overhead. Stack allocation, fixed arrays, non-owning fat slices, and raw pointers deliver predictable execution.
- **Explicit Types and Hardware Access**: Fixed-width integer types, pointers, slices, and volatile I/O primitives support systems programming.
- **1-Based Indexing**: Natural mathematical indexing (1..Length) across arrays, slices, and strings, safeguarded by static analysis and runtime bounds checks.
- **Turnkey RP2040 Bare-Metal Support**: Built-in scaffolding for Raspberry Pi Pico firmware, including Boot2 stage, vector tables, linker scripts, and direct generation of UF2 bootloader images.
- **Pre-Integrated Hardware HAL**: Comprehensive 16-peripheral hardware abstraction layer (Clock, GPIO, UART, SPI, I2C, ADC, PWM, DMA, Watchdog, Multicore, PIO, RTC, Interp, System, and Time).
- **C ABI Interoperability**: Direct foreign function linkage enabling frictionless communication with existing C libraries.
- **Integrated Toolchain**: Includes the unified `loom` project tool, the optimizing `loomc` compiler, and the `loom-lsp` Language Server Protocol service.

### RP2040 peripheral handles

Initialize I²C or SPI with the pins you wired. Loom chooses the controller and returns a typed handle; pass that handle to a device library so it keeps using the bus you selected.

```loom
Module Main
Import I2C
Import RP2040
Import SSD1306
use I2C
use RP2040
use SSD1306
Block Main Declares
    OLED_I2C : I2C_Handle
Implements
    RP2040_Init()
    OLED_I2C = I2C_Init(4, 5)
    SSD1306_Init(OLED_I2C)
End Main
```

I²C defaults to 100 kHz. SPI defaults to 1 MHz, eight-bit words, mode 0. I²C/SPI and the bundled SSD1306/MPU6050 operations capture failures internally and keep a readable diagnostic for debugger inspection. See the RP2040 reference and SDK README for handle types, defaults, and current limitations.

---

## Downloads & Toolchain Management

- **macOS Desktop Native Toolchain (`Loom-1.2.0-macos-arm64-native.pkg`)**: Apple Silicon installer with the Loom build driver, native compiler, language server, standard library, and bundled compiler runtime libraries.
- **RP2040 Embedded Toolchain (`Loom-1.2.0-macos-arm64-rp2040.pkg`)**: Apple Silicon installer with the Loom build driver, ARM Cortex-M0+ compiler, language server, `llvm-objcopy` for UF2 generation, current RP2040 BSP/startup/linker files, standard library, and bundled compiler runtime libraries. macOS Command Line Tools provide `clang`; bare-metal linking also requires `ld.lld` on `PATH`.
- **Independent Coexistence & Toolchain Management**: Both packages install independently into `/usr/local/share/loom/toolchains/` without conflicts. Manage and switch toolchains easily:
  ```bash
  # View all installed toolchains and active default
  loom toolchains

  # Switch the active default toolchain
  loom use rp2040
  loom use native

  # Override toolchain for a single build
  loom build --toolchain rp2040
  ```
  Loom's native toolchain strictly prevents building embedded RP2040 projects and guides you to switch to the RP2040 toolchain.
- **VS Code Extension**: The official VS Code extension for Loom language support is available in this repository as a prepackaged `.vsix` file ([`vscode-loom/Loom-1.2.0.vsix`](vscode-loom/Loom-1.2.0.vsix)):
  - **Syntax Highlighting**: Complete TextMate grammar covering blocks, keywords, built-in types, operators, and string interpolation.
  - **Editor Productivity**: Automated code snippets, bracket autocompletion, intelligent indentation rules, and file icon associations.
  - **Compiler-Backed Diagnostics**: Real-time error reporting and inline diagnostic squiggles.
  - **Direct Installation**: Install the prepackaged `.vsix` directly in VS Code via the Extensions view (`Install from VSIX...`) or command line.

---

## Comprehensive Documentation

For in-depth explanations, installation steps, step-by-step tutorials, code examples, compiler internals, and complete API references, explore the online documentation at [thirstymelon.github.io/Loom](https://thirstymelon.github.io/Loom/) or the offline suite in `docs/`:

- [**Documentation Hub**](https://thirstymelon.github.io/Loom/): Central gateway, search, and feature roadmap.
- [**Getting Started Guide**](https://thirstymelon.github.io/Loom/getting-started.html): Installation, toolchain setup, workspace management, and building your first program.
- [**Beginner's Course (10 Lessons)**](https://thirstymelon.github.io/Loom/tutorials/beginner-guide.html): A complete tutorial series taking newcomers from basic syntax to advanced systems programming.
- [**RP2040 Bare-Metal Guide**](https://thirstymelon.github.io/Loom/embedded-rp2040.html): Pinouts, hardware architecture, register maps, SDK HAL, and flashing instructions.
- [**Language Reference Manual**](https://thirstymelon.github.io/Loom/reference.html): Formal syntax specifications, keyword catalog, types, operator precedence, and memory model.
- [**Developer & Compiler Overview**](https://thirstymelon.github.io/Loom/developer-overview.html): Architecture of the frontend parser, AST lowering, LLVM code generator, and optimization pipeline.

*The entire documentation suite is standalone and fully functional offline. Simply open `docs/index.html` in any web browser.*

## Install libraries and toolchains with `loom`

Public Loom releases publish a checksummed package catalog and downloadable archives on GitHub Releases. Libraries are installed per-user under `~/.loom/libraries` and become available to Loom projects automatically; toolchains are installed under `~/.loom/toolchains` without requiring an administrator password.

```bash
loom install <library>
loom libraries
loom uninstall <library>

loom install toolchain rp2040
loom toolchains
loom use rp2040
```

New release tags run the package workflow in `.github/workflows/publish-release.yml`. The initial hosted toolchain bundles target macOS Apple Silicon; the library archives are platform-independent. Set `LOOM_REGISTRY_URL` to use a compatible self-hosted TSV catalog. Downloads are checked against the catalog's SHA-256 digest before installation.

---

## License

The Loom Programming Language and toolchain are open-source software licensed under the **Apache License 2.0**. Refer to the [LICENSE](LICENSE) file for complete terms.
