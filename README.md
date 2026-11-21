<div align="center">

<img src="logo/Loom_Logo.png" alt="Loom Language Logo" width="140" height="140" />

# Loom Programming Language

**A modern, deterministic, high-performance systems programming language with an optimizing LLVM backend.**

[![Version](https://img.shields.io/badge/version-1.4.0-92a9e1.svg)](https://github.com/thirstymelon/Loom/releases)
[![Backend](https://img.shields.io/badge/backend-LLVM%20Native-blue.svg)](https://llvm.org/)
[![Libraries](https://img.shields.io/badge/libraries-loom--libraries-orange.svg)](https://github.com/thirstymelon/loom-libraries)
[![VS Code](https://img.shields.io/badge/vscode-extension-informational.svg)](vscode-loom/)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

[Documentation](https://thirstymelon.github.io/Loom/) • [Getting Started](https://thirstymelon.github.io/Loom/start.html) • [Learn Loom](https://thirstymelon.github.io/Loom/learn.html) • [Language Reference](https://thirstymelon.github.io/Loom/reference.html) • [Libraries Repository](https://github.com/thirstymelon/loom-libraries)

</div>

---

## Overview

Loom is a statically typed, block-oriented systems programming language engineered for safety, speed, and readability. It couples clean Pascal- and Ada-inspired declarative grammar with modern compiler capabilities powered by LLVM.

- **Optimizing LLVM Backend**: Generates vector-optimized native machine code with link-time optimization (LTO) support.
- **Deterministic Memory Model**: Compile-time borrow checking, lifetime tracking, and RAII destruction for deterministic resource cleanup.
- **Modern Concurrency & Async**: Structured concurrency primitives, task spawning, mutexes, and typed channels.
- **Modern Expressiveness**: Pattern matching with guards, higher-order closures, uniform collections, and operator overloading.
- **First-Class Developer Tooling**: Built-in test runner, automatic code formatter (`loom fmt`), HTML documentation generator (`loom doc`), and DWARF debug symbols.

---

## Libraries & Package Management

Official Loom libraries and frameworks are hosted in the [loom-libraries](https://github.com/thirstymelon/loom-libraries) repository.

Install packages directly using the `loom get` command:

```bash
loom get <library>
```

For example, to install the cryptographic hash library:
```bash
loom get hash
```

Installed libraries are placed under `~/.loom/libraries` and are automatically recognized by the compiler and language server across all your projects.

- **Libraries Repository**: [https://github.com/thirstymelon/loom-libraries](https://github.com/thirstymelon/loom-libraries)
- **List Installed Libraries**: `loom libraries`
- **Uninstall a Library**: `loom uninstall <library>`

---

## Editor & Language Server Support

The official Loom extension for Visual Studio Code is located in [vscode-loom/](vscode-loom/) and provides:

- **Syntax Highlighting**: Full TextMate grammar and semantic token highlighting.
- **Intelligent Autocompletion**: Context-aware completions for blocks, types, records, and keywords.
- **Navigation**: Go-to-definition, find all references, and workspace symbol search.
- **Refactoring & Diagnostics**: Real-time compiler diagnostics and workspace-wide symbol renaming.
- **Formatting**: Integrated auto-formatting via `loom fmt`.

Install the extension directly via `vscode-loom/Loom-1.4.0.vsix` or from within VS Code.

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
