# Loom Toolchain Build & Packaging Scripts

This directory contains automation scripts for packaging the Loom toolchain across its two streams.

## Multi-Toolchain Packaging (`build_pkg.sh`)

Loom packages two distinct macOS Apple Silicon (`arm64`) installer packages (`.pkg`):

1. **Desktop Native Toolchain (`Loom-1.1.0-macos-arm64-native.pkg`)**:
   - Universal build driver CLI (`/usr/local/bin/loom`).
   - Native desktop compiler (`/usr/local/share/loom/toolchains/native/bin/loomc`) targeting host native architecture (`arm64-apple-darwin`).
   - Toolchain manifest (`toolchain.toml`) with native desktop constraints.
   - Desktop standard library (`/usr/local/share/loom/toolchains/native/sdk/std/`).

2. **RP2040 Embedded Toolchain (`Loom-1.1.0-macos-arm64-rp2040.pkg`)**:
   - Universal build driver CLI (`/usr/local/bin/loom`).
   - Embedded compiler (`/usr/local/share/loom/toolchains/rp2040/bin/loomc`) with ARM Cortex-M0+ (`thumbv6m-none-eabi`) backend support.
   - Toolchain manifest (`toolchain.toml`) configuring RP2040 bare-metal constraints.
   - Bare-metal BSP and hardware drivers (`/usr/local/share/loom/toolchains/rp2040/sdk/targets/rp2040/`).
   - Embedded standard library (`/usr/local/share/loom/toolchains/rp2040/sdk/std/`).

Both packages install side-by-side into `/usr/local/share/loom/toolchains/` without collision. The `loom` CLI provides management and switching commands:
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
