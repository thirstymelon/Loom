<div align="center">

<img src="logo/Loom_Logo.png" alt="Loom Language Logo" width="140" height="140" />

# Loom Programming Language

**A modern, deterministic, high-performance systems programming language with an LLVM backend and first-class bare-metal support.**

[![Version](https://img.shields.io/badge/version-1.1.0-92a9e1.svg)](https://github.com/thirstymelon/Loom/releases)
[![Backend](https://img.shields.io/badge/backend-LLVM%20Native-blue.svg)](https://llvm.org/)
[![Embedded](https://img.shields.io/badge/target-RP2040%20Bare--Metal-brightgreen.svg)](https://thirstymelon.github.io/Loom/embedded-rp2040.html)
[![VS Code](https://img.shields.io/badge/vscode-extension%20in%20repo-informational.svg)](vscode-loom/)
[![macOS DMG](https://img.shields.io/badge/macos-dmg%20in%20releases-lightgrey.svg)](https://github.com/thirstymelon/Loom/releases)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

[Documentation Hub](https://thirstymelon.github.io/Loom/) • [Getting Started](https://thirstymelon.github.io/Loom/getting-started.html) • [Beginner's Guide](https://thirstymelon.github.io/Loom/tutorials/beginner-guide.html) • [RP2040 Bare-Metal](https://thirstymelon.github.io/Loom/embedded-rp2040.html) • [Language Reference](https://thirstymelon.github.io/Loom/reference.html)

</div>

---

## Introduction

Loom is a block-oriented, statically typed systems programming language designed for developers who demand the deterministic execution, raw speed, and direct hardware control of C, combined with clean, readable, block-structured syntax and modern safety guarantees.

Featuring a platform-independent C++17 frontend and an optimizing LLVM native-code backend, Loom parses, validates, and directly lowers source files into optimized LLVM Intermediate Representation and machine code without intermediate C source wrappers. Loom operates with zero garbage collection overhead and provides explicit fixed-width data types, deterministic LIFO resource cleanup, natural 1-based indexing for collections, and native bare-metal support for ARM Cortex-M0+ microcontrollers like the Raspberry Pi Pico (RP2040).

---

## Core Capabilities

- **Direct LLVM Backend**: Generates target-native object code with comprehensive LLVM optimization passes (-O3), inlining, and target vectorization.
- **Deterministic Memory Model**: Free of garbage collection pauses and hidden runtime overhead. Stack allocation, fixed arrays, non-owning fat slices, and raw pointers deliver predictable execution.
- **Deterministic Cleanup**: Last-In First-Out (LIFO) scoped cleanup execution guarantees timely finalization of system resources.
- **1-Based Indexing**: Natural mathematical indexing (1..Length) across arrays, slices, and strings, safeguarded by static analysis and runtime bounds checks.
- **Turnkey RP2040 Bare-Metal Support**: Built-in scaffolding for Raspberry Pi Pico firmware, including Boot2 stage, vector tables, linker scripts, and direct generation of UF2 bootloader images.
- **Pre-Integrated Hardware HAL**: Comprehensive 16-peripheral hardware abstraction layer (Clock, GPIO, UART, SPI, I2C, ADC, PWM, DMA, Watchdog, Multicore, PIO, RTC, Interp, System, and Time).
- **C ABI Interoperability**: Direct foreign function linkage enabling frictionless communication with existing C libraries.
- **Integrated Toolchain**: Includes the unified `loom` project tool, the optimizing `loomc` compiler, and the `loom-lsp` Language Server Protocol service.

---

## Downloads & VS Code Extension

- **macOS Desktop Native Toolchain (`Loom-1.1.0-macos-arm64-native.dmg`)**: Prebuilt Apple Silicon disk image attached to [GitHub Releases](https://github.com/thirstymelon/Loom/releases), providing an installer for native desktop application development (optimizing compiler `loomc`, workspace build driver `loom`, standard library `sdk/std/`, and the VS Code extension).
- **RP2040 Embedded Toolchain (`Loom-1.1.0-macos-arm64-rp2040.dmg`)**: Dedicated bare-metal embedded disk image attached to [GitHub Releases](https://github.com/thirstymelon/Loom/releases), providing the complete Raspberry Pi Pico toolchain (ARM Cortex-M0+ `loomc`, build driver with automatic UF2 packager `loom`, full 16-peripheral RP2040 HAL/BSP, `boot.s`, `linker.ld`, and the VS Code extension).
- **VS Code Extension**: The official VS Code extension for Loom language support is bundled directly within this repository as a prepackaged `.vsix` file ([`vscode-loom/Loom-1.1.0.vsix`](vscode-loom/Loom-1.1.0.vsix)):
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

---

## License

The Loom Programming Language and toolchain are open-source software licensed under the **Apache License 2.0**. Refer to the [LICENSE](LICENSE) file for complete terms.
