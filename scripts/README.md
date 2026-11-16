# Loom Toolchain Build & Packaging Scripts

This directory contains automation scripts for packaging the Loom toolchain across its two streams.

## Multi-Toolchain Packaging (`build_pkg.sh`)

Loom packages two distinct macOS Apple Silicon (`arm64`) installer packages (`.pkg`):

1. **Desktop Native Toolchain (`Loom-1.3.0-macos-arm64-native.pkg`)**:
   - Universal build driver CLI (`/usr/local/bin/loom`) and Language Server (`/usr/local/bin/loom-lsp`).
   - Native desktop compiler (`/usr/local/share/loom/toolchains/native/bin/loomc`) targeting host native architecture (`arm64-apple-darwin`).
   - Bundled Z3 and zstd runtime libraries, so the compiler does not need Homebrew's runtime paths.
   - Toolchain manifest (`toolchain.toml`) with native desktop constraints.
   - Desktop standard library (`/usr/local/share/loom/toolchains/native/sdk/std/`).

2. **RP2040 Embedded Toolchain (`Loom-1.3.0-macos-arm64-rp2040.pkg`)**:
   - Universal build driver CLI (`/usr/local/bin/loom`) and Language Server (`/usr/local/bin/loom-lsp`).
   - Embedded compiler (`/usr/local/share/loom/toolchains/rp2040/bin/loomc`) with ARM Cortex-M0+ (`thumbv6m-none-eabi`) backend support.
   - `llvm-objcopy` for generating UF2 firmware images, plus its runtime libraries.
   - Toolchain manifest (`toolchain.toml`) configuring RP2040 bare-metal constraints.
   - Bare-metal BSP and hardware drivers (`/usr/local/share/loom/toolchains/rp2040/sdk/targets/rp2040/`).
   - Embedded standard library (`/usr/local/share/loom/toolchains/rp2040/sdk/std/`).

Both packages include their non-system macOS runtime libraries and install side-by-side into `/usr/local/share/loom/toolchains/` without collision. The macOS Command Line Tools provide `clang`; bare-metal linking also requires `ld.lld` on `PATH`. The independent SSD1306 and MPU6050 library archives are published separately and remain installable with `loom install`.

The `loom` CLI provides management and switching commands:
- `loom toolchains`: list all installed toolchains and show the active default.
- `loom use <toolchain>`: switch the active default toolchain (e.g. `loom use rp2040`, `loom use native`).
- `loom build --toolchain <toolchain>`: override toolchain per-build.

### Usage

```bash
# Build both toolchain PKGs (default)
./scripts/build_pkg.sh all

# Build only the desktop native toolchain PKG
./scripts/build_pkg.sh native

# Build only the RP2040 embedded toolchain PKG
./scripts/build_pkg.sh rp2040
```

The generated `.pkg` files are written to the repository root. The builder compiles the current Loom sources before packaging, so rerun it after changing compiler or SDK code.
