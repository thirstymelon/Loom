<div align="center">

<img src="logo/Loom_Logo.png" alt="Loom Language Logo" width="140" height="140" />

# Loom Programming Language

**A modern, deterministic, high-performance systems programming language with an LLVM backend and first-class bare-metal support.**

[![Version](https://img.shields.io/badge/version-1.0.0-92a9e1.svg)](https://github.com/thirstymelon/Loom/releases)
[![Backend](https://img.shields.io/badge/backend-LLVM%20Native-blue.svg)](https://llvm.org/)
[![Embedded](https://img.shields.io/badge/target-RP2040%20Bare--Metal-brightgreen.svg)](docs/embedded-rp2040.html)
[![VS Code](https://img.shields.io/badge/vscode-extension%20available-informational.svg)](vscode-loom/)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

[Documentation Hub](docs/index.html) • [Getting Started](docs/getting-started.html) • [Beginner's Guide](docs/tutorials/beginner-guide.html) • [RP2040 Bare-Metal](docs/embedded-rp2040.html) • [Language Reference](docs/reference.html)

</div>

---

## Introduction

Loom is a block-oriented, statically typed systems programming language designed for developers who demand the deterministic execution, raw speed, and direct hardware control of C, combined with clean, readable, block-structured syntax and modern safety guarantees.

Featuring a platform-independent C++17 frontend and an optimizing LLVM native-code backend, Loom parses, validates, and directly lowers source files into optimized LLVM Intermediate Representation and machine code without intermediate C source wrappers. Loom operates with zero garbage collection overhead and provides explicit fixed-width data types, deterministic LIFO resource cleanup, natural 1-based indexing for collections, and native bare-metal support for ARM Cortex-M0+ microcontrollers like the Raspberry Pi Pico (RP2040).

---

## Project Structure

```text
Loom/
├── CMakeLists.txt        # Top-level CMake build system definition
├── LICENSE               # Apache License 2.0 terms
├── README.md             # Project introduction and repository overview
├── logo/                 # Official Loom branding assets and icons
│   └── Loom_Logo.png
├── docs/                 # Complete standalone offline documentation suite
│   ├── index.html        # Documentation hub & search
│   ├── getting-started.html # Installation, CLI reference, and toolchain setup
│   ├── developer-overview.html # Compiler pipeline, AST, and lowering architecture
│   ├── embedded-rp2040.html # Bare-metal RP2040 hardware and HAL guide
│   ├── reference.html    # Formal lexical grammar and language specification
│   ├── styles.css        # Documentation styling and theme configuration
│   └── tutorials/
│       ├── index.html    # Tutorial track roadmap
│       └── beginner-guide.html # Comprehensive 10-lesson beginner course
├── scripts/              # Distribution, packaging, and DMG creation scripts
│   └── build_dmg.sh
├── sdk/                  # Loom Standard Development Kit
│   ├── std/              # Core standard library modules (I/O, Math)
│   └── targets/          # Embedded target support
│       └── rp2040/       # RP2040 BSP (System, GPIO, Time, UART, ADC, PWM)
├── src/                  # Native C++ compiler and workspace driver source
│   ├── main.cpp          # Compiler frontend and LLVM IR generator (loomc)
│   └── driver.cpp        # Unified workspace driver and CLI tool (loom)
├── tests/                # Test suites, benchmarks, and validation tests
│   ├── benchmarks.py     # Comparative benchmarks (Loom vs C vs Rust)
│   ├── docs_tests.py     # Documentation verification test harness
│   ├── driver_tests.py   # Workspace CLI integration test suite
│   └── modern_features_tests.py # Modern language features verification
└── vscode-loom/          # Official Visual Studio Code extension
    ├── Loom-1.0.0.vsix   # Prepackaged ready-to-install VS Code extension
    ├── package.json      # Extension manifest and contributions
    ├── language-configuration.json # Bracket matching and indentation rules
    ├── syntaxes/         # TextMate grammar for semantic syntax highlighting
    └── snippets/         # IDE code completion snippets
```

---

## Core Capabilities

- **Direct LLVM Backend**: Generates target-native object code with comprehensive LLVM optimization passes (-O3), inlining, and target vectorization.
- **Deterministic Memory Model**: Free of garbage collection pauses and hidden runtime overhead. Stack allocation, fixed arrays, non-owning fat slices, and raw pointers deliver predictable execution.
- **Deterministic Cleanup**: Last-In First-Out (LIFO) scoped cleanup execution guarantees timely finalization of system resources.
- **1-Based Indexing**: Natural mathematical indexing (1..Length) across arrays, slices, and strings, safeguarded by static analysis and runtime bounds checks.
- **Turnkey RP2040 Bare-Metal Support**: Built-in scaffolding for Raspberry Pi Pico firmware, including Boot2 stage, vector tables, linker scripts, and direct generation of UF2 bootloader images.
- **Pre-Integrated Hardware HAL**: Modular SDK libraries for System clocks, GPIO, Time, UART serial communications, ADC, and PWM.
- **C ABI Interoperability**: Direct foreign function linkage enabling frictionless communication with existing C libraries.
- **Integrated Toolchain**: Includes the unified `loom` project tool, the optimizing `loomc` compiler, and the `loom-lsp` Language Server Protocol service.

---

## VS Code Extension for Loom

An official VS Code extension is available for full Loom language support:
- **Syntax Highlighting**: Complete TextMate grammar covering blocks, keywords, built-in types, operators, and string interpolation.
- **Editor Productivity**: Automated code snippets, bracket autocompletion, intelligent indentation rules, and file icon associations.
- **Compiler-Backed Diagnostics**: Inline error reporting and diagnostic squiggles directly inside the editor.
- **Direct Installation**: The repository includes the prepackaged extension file (`vscode-loom/Loom-1.0.0.vsix`), which can be installed in VS Code via the Extensions view or command line.

---

## Comprehensive Documentation

For in-depth explanations, installation steps, step-by-step tutorials, code examples, compiler internals, and complete API references, explore the included documentation:

- [**Documentation Hub**](docs/index.html): Central gateway, search, and feature roadmap.
- [**Getting Started Guide**](docs/getting-started.html): Installation, toolchain setup, workspace management, and building your first program.
- [**Beginner's Course (10 Lessons)**](docs/tutorials/beginner-guide.html): A complete tutorial series taking newcomers from basic syntax to advanced systems programming.
- [**RP2040 Bare-Metal Guide**](docs/embedded-rp2040.html): Pinouts, hardware architecture, register maps, SDK HAL, and flashing instructions.
- [**Language Reference Manual**](docs/reference.html): Formal syntax specifications, keyword catalog, types, operator precedence, and memory model.
- [**Developer & Compiler Overview**](docs/developer-overview.html): Architecture of the frontend parser, AST lowering, LLVM code generator, and optimization pipeline.

*The entire documentation suite is standalone and fully functional offline. Simply open `docs/index.html` in any web browser.*

---

## License

The Loom Programming Language and toolchain are open-source software licensed under the **Apache License 2.0**. Refer to the [LICENSE](LICENSE) file for complete terms.
