# Loom Toolchain Build & Packaging Scripts

This directory contains automation scripts for packaging the Loom toolchain across its two streams.

## Multi-Toolchain Packaging (`build_dmg.sh`)

Loom packages two distinct macOS Apple Silicon (`arm64`) disk images:

1. **Desktop Native Toolchain (`Loom-1.1.0-macos-arm64-native.dmg`)**:
   - Compiler (`loomc`) targeting host native architecture (`arm64-apple-darwin`).
   - Project driver (`loom`) for host desktop applications.
   - Standard library (`sdk/std/`).
   - Official VS Code extension (`vscode/Loom-1.1.0.vsix`).
   - GUI package installer (`.pkg`) and terminal install script (`install.sh`).

2. **RP2040 Embedded Toolchain (`Loom-1.1.0-macos-arm64-rp2040.dmg`)**:
   - Compiler (`loomc`) with ARM Cortex-M0+ (`thumbv6m-none-eabi`) backend support.
   - Project driver (`loom`) configured with RP2040 scaffolding (`--rp2040`) and UF2 packaging.
   - Bare-metal BSP and hardware drivers (`sdk/targets/rp2040/`).
   - Official VS Code extension (`vscode/Loom-1.1.0.vsix`).
   - GUI package installer (`.pkg`) and terminal install script (`install.sh`).

### Usage

```bash
# Build both toolchain DMGs (default)
./scripts/build_dmg.sh all

# Build only the desktop native toolchain DMG
./scripts/build_dmg.sh native

# Build only the RP2040 embedded toolchain DMG
./scripts/build_dmg.sh rp2040
```

