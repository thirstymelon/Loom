<div align="center">

<img src="logo/Loom_Logo.png" alt="Loom Language Logo" width="140" height="140" />

# Loom Programming Language

**A modern, deterministic, high-performance systems programming language with an optimizing LLVM backend.**

[![Version](https://img.shields.io/badge/version-1.5.0-92a9e1.svg)](https://github.com/thirstymelon/Loom/releases)
[![Backend](https://img.shields.io/badge/backend-LLVM%20Native-blue.svg)](https://llvm.org/)
[![Libraries](https://img.shields.io/badge/libraries-loom--libraries-orange.svg)](https://github.com/thirstymelon/loom-libraries)
[![VS Code](https://img.shields.io/badge/vscode-extension-informational.svg)](vscode-loom/)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

[Documentation](https://thirstymelon.github.io/Loom/) • [Getting Started](https://thirstymelon.github.io/Loom/start.html) • [Learn Loom](https://thirstymelon.github.io/Loom/learn.html) • [Language Reference](https://thirstymelon.github.io/Loom/reference.html) • [Libraries Repository](https://github.com/thirstymelon/loom-libraries)

The repository's conventions for block layout, naming, expressions, collection iteration, and performance-sensitive code are in [`docs/style.md`](docs/style.md).

For short-lived bulk allocations, the runtime exposes a single-threaded bump arena. Its raw pointers become invalid after reset or destroy; usage and C ABI declarations are in [`docs/arenas.md`](docs/arenas.md). The compiler can emit machine-readable diagnostics with `loomc --diagnostic-format=json`; see the [diagnostic format and code families](docs/compiler-diagnostics.md). Benchmark methodology and current Loom/C/Rust results are in [`benchmarks/performance_report_2026-11-23.md`](benchmarks/performance_report_2026-11-23.md).

</div>

---

## Overview

Loom is a statically typed, block-oriented systems programming language engineered for safety, speed, and readability. It couples clean Pascal- and Ada-inspired declarative grammar with modern compiler capabilities powered by LLVM.

- **Optimizing LLVM Backend**: Generates vector-optimized native machine code with link-time optimization (LTO) support.
- **Explicit Resource Safety**: Compile-time checks for ownership moves, read-only borrows, common use-after-free cases, and RAII `Drop` cleanup. Raw pointers remain unsafe and are not fully lifetime-checked.
- **Modern Concurrency & Async**: Structured concurrency primitives, task spawning, mutexes, and typed channels.
- **Implicit Data Parallelism**: Large hosted Float32 vector operations use a persistent worker pool without application-level parallel annotations. See [`docs/implicit-parallelism.md`](docs/implicit-parallelism.md).
- **Modern Expressiveness**: Pattern matching with guards, higher-order closures, uniform collections, and operator overloading.
- **First-Class Developer Tooling**: Built-in test runner, automatic code formatter (`loom fmt`), HTML documentation generator (`loom doc`), and DWARF debug symbols.

---

## Libraries & Package Management (v1.5.0)

Loom features a modern, deterministic package manager supporting Semantic Versioning (SemVer), transitive dependency resolution with conflict detection, local path dependencies, git repositories, and cryptographic integrity enforcement via `config.lock`.

### Project Initialization & Adding Dependencies
```bash
# Initialize a new native project or RP2040 bare-metal project in the current directory
loom init
loom init --rp2040

# Add dependencies with SemVer constraint solving
loom add MathLib                     # Resolves latest compatible version (^1.4.0)
loom add SensorLib --path ../SensorLib  # Local filesystem path dependency
loom add NetLib --git https://github.com/org/NetLib.git --branch main  # Git repository dependency
```

### Dependency Resolution & Lockfile Integrity
Run `loom update` to resolve dependencies, compute cryptographic directory digests (SHA-256), and emit an immutable `config.lock`.
On every build, Loom verifies that installed packages match the exact checksums in `config.lock` to prevent supply-chain tampering.

### Embedded & Hardware Tools
```bash
# Flash compiled UF2/ELF to connected Raspberry Pi Pico
loom flash

# Attach directly to USB-CDC serial console after flashing
loom flash --monitor

# Interactive serial monitor
loom monitor
```

### Registry & Publishing
- **Search Registry**: `loom search <query>`
- **Publish Package/Toolchain**: `loom publish`
- **Installed Libraries**: `loom libraries`
- **Uninstall a Library**: `loom uninstall <library>`

---

## Editor & Language Server Support

The official Loom extension for Visual Studio Code is located in [vscode-loom/](vscode-loom/) and provides:

- **Syntax Highlighting**: Full TextMate grammar and semantic token highlighting.
- **Intelligent Autocompletion**: Context-aware completions for blocks, types, records, and keywords.
- **Navigation**: Go-to-definition, find all references, and workspace symbol search.
- **Refactoring & Diagnostics**: Real-time compiler diagnostics and workspace-wide symbol renaming.
- **Formatting**: Integrated auto-formatting via `loom fmt`.

Install the extension directly via `vscode-loom/Loom-1.5.0.vsix` or from within VS Code.

---

## Documentation

Full tutorials, language specifications, and architecture guides are available online and offline:

- [**Overview & Philosophy**](https://thirstymelon.github.io/Loom/index.html)
- [**Getting Started**](https://thirstymelon.github.io/Loom/start.html)
- [**Learn Loom**](https://thirstymelon.github.io/Loom/learn.html)
- [**Language Reference**](https://thirstymelon.github.io/Loom/reference.html)
- [**Compiler Internals & Target Architecture**](https://thirstymelon.github.io/Loom/internals.html)

All documentation is bundled for offline access in the `docs/` directory (`docs/index.html`).

---

## License

The Loom Programming Language and toolchain are open-source software licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for details.

## Networking

Loom's native networking operations are lowered by the compiler to the host socket ABI; the networking implementation does not use a C runtime source file or `Extern Block`. See [the networking guide](docs/networking.md) for TCP/UDP byte APIs, the Loom `Net` module, platform support, and current limits.
