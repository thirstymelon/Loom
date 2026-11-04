#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/.build"
VERSION="1.1.0"

TARGET_FILTER="${1:-all}"

echo "=========================================================="
echo " Loom Multi-Toolchain DMG Builder for macOS Apple Silicon "
echo " Version: $VERSION | Target Mode: $TARGET_FILTER"
echo "=========================================================="

echo "=== Building Loom binaries ==="
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -B "$BUILD_DIR" -S "$REPO_ROOT"
fi
cmake --build "$BUILD_DIR" -j8
mkdir -p "$REPO_ROOT/bin"
cp -f "$BUILD_DIR/loom" "$REPO_ROOT/bin/loom"
cp -f "$BUILD_DIR/loomc" "$REPO_ROOT/bin/loomc"
chmod 755 "$REPO_ROOT/bin/loom" "$REPO_ROOT/bin/loomc"

echo "=== Packaging VS Code extension ==="
if [ ! -f "$REPO_ROOT/vscode-loom/Loom-1.1.0.vsix" ]; then
    (cd "$REPO_ROOT/vscode-loom" && npx --yes @vscode/vsce package -o Loom-1.1.0.vsix --allow-missing-repository --no-git-tag-version)
fi

# ==============================================================================
# 1. Desktop Native Toolchain: Loom-${VERSION}-macos-arm64-native.dmg
# ==============================================================================
build_native_dmg() {
    echo ""
    echo "===================================================================="
    echo " Building Toolchain 1: macOS Apple Silicon Desktop Native           "
    echo " Package: Loom-${VERSION}-macos-arm64-native.dmg                    "
    echo "===================================================================="
    local NATIVE_PKG_ROOT="$BUILD_DIR/pkg_root_native"
    local NATIVE_DMG_STAGING="$BUILD_DIR/dmg_staging_native"
    local NATIVE_DMG_NAME="Loom-${VERSION}-macos-arm64-native.dmg"
    local NATIVE_DMG_PATH="$REPO_ROOT/$NATIVE_DMG_NAME"

    rm -rf "$NATIVE_PKG_ROOT" "$NATIVE_DMG_STAGING"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/bin"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/share/loom/sdk/std"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/share/loom/vscode"

    # Binaries
    cp -f "$REPO_ROOT/bin/loom" "$NATIVE_PKG_ROOT/usr/local/bin/loom"
    cp -f "$REPO_ROOT/bin/loomc" "$NATIVE_PKG_ROOT/usr/local/bin/loomc"
    chmod 755 "$NATIVE_PKG_ROOT/usr/local/bin/loom" "$NATIVE_PKG_ROOT/usr/local/bin/loomc"

    # Desktop standard library only (no embedded targets)
    cp -Rf "$REPO_ROOT/sdk/std/"* "$NATIVE_PKG_ROOT/usr/local/share/loom/sdk/std/"
    cp -f "$REPO_ROOT/vscode-loom/Loom-1.1.0.vsix" "$NATIVE_PKG_ROOT/usr/local/share/loom/vscode/Loom-1.1.0.vsix"

    echo "  -> Building installer package (.pkg)..."
    mkdir -p "$NATIVE_DMG_STAGING"
    pkgbuild --root "$NATIVE_PKG_ROOT" \
             --identifier "org.loom.lang.native" \
             --version "$VERSION" \
             --install-location "/" \
             "$NATIVE_DMG_STAGING/Install Loom Desktop (Apple Silicon Native).pkg"

    echo "  -> Assembling DMG staging directory..."
    mkdir -p "$NATIVE_DMG_STAGING/bin"
    cp -f "$REPO_ROOT/bin/loom" "$NATIVE_DMG_STAGING/bin/loom"
    cp -f "$REPO_ROOT/bin/loomc" "$NATIVE_DMG_STAGING/bin/loomc"
    chmod 755 "$NATIVE_DMG_STAGING/bin/loom" "$NATIVE_DMG_STAGING/bin/loomc"

    mkdir -p "$NATIVE_DMG_STAGING/sdk/std"
    cp -Rf "$REPO_ROOT/sdk/std/"* "$NATIVE_DMG_STAGING/sdk/std/"

    mkdir -p "$NATIVE_DMG_STAGING/vscode"
    cp -f "$REPO_ROOT/vscode-loom/Loom-1.1.0.vsix" "$NATIVE_DMG_STAGING/vscode/Loom-1.1.0.vsix"

    cat << 'EOF' > "$NATIVE_DMG_STAGING/install.sh"
#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Installing Loom Desktop Native Toolchain for macOS Apple Silicon (arm64)..."
sudo mkdir -p /usr/local/bin
sudo mkdir -p /usr/local/share/loom/sdk/std
sudo mkdir -p /usr/local/share/loom/vscode

echo "  -> Installing compiler (loomc) and build driver (loom) to /usr/local/bin"
sudo cp -f "$DIR/bin/loom" /usr/local/bin/loom
sudo cp -f "$DIR/bin/loomc" /usr/local/bin/loomc
sudo chmod 755 /usr/local/bin/loom /usr/local/bin/loomc

echo "  -> Installing Desktop standard library to /usr/local/share/loom/sdk/std"
sudo cp -Rf "$DIR/sdk/std/"* /usr/local/share/loom/sdk/std/

echo "  -> Copying VS Code extension to /usr/local/share/loom/vscode"
sudo cp -f "$DIR/vscode/Loom-1.1.0.vsix" /usr/local/share/loom/vscode/

echo ""
echo "Loom Desktop Native Toolchain successfully installed!"
echo "Verify installation:"
echo "  loom --version"
echo "  loomc --version"
echo ""
echo "To install the VS Code extension:"
echo "  code --install-extension /usr/local/share/loom/vscode/Loom-1.1.0.vsix"
EOF
    chmod 755 "$NATIVE_DMG_STAGING/install.sh"

    cat << 'EOF' > "$NATIVE_DMG_STAGING/uninstall.sh"
#!/usr/bin/env bash
set -euo pipefail
echo "Uninstalling Loom Desktop Native Toolchain..."
sudo rm -f /usr/local/bin/loom
sudo rm -f /usr/local/bin/loomc
sudo rm -rf /usr/local/share/loom/sdk/std
sudo rm -f /usr/local/share/loom/vscode/Loom-1.1.0.vsix
echo "Loom Desktop Native Toolchain uninstalled."
EOF
    chmod 755 "$NATIVE_DMG_STAGING/uninstall.sh"

    cat << 'EOF' > "$NATIVE_DMG_STAGING/README.txt"
================================================================================
Loom Programming Language - macOS Apple Silicon Native Toolchain
Version 1.1.0
================================================================================

This package contains the high-performance desktop native toolchain for Loom,
designed for building native macOS Apple Silicon applications with zero garbage
collection overhead, deterministic memory safety, and native LLVM optimizations.

CONTENTS:
---------
- Compiler:      /usr/local/bin/loomc
- Build Driver:  /usr/local/bin/loom
- Standard Lib:  /usr/local/share/loom/sdk/std/ (math.loom)
- VS Code VSIX:  /usr/local/share/loom/vscode/Loom-1.1.0.vsix

INSTALLATION OPTIONS:
---------------------
1. GUI Package Installer (Recommended):
   Double-click "Install Loom Desktop (Apple Silicon Native).pkg" and follow the prompts.

2. Terminal Script:
   Open Terminal in this mounted disk image and run:
     sudo ./install.sh

3. Portable / Zero-Install Usage:
   You can run directly from this disk image or copy 'bin/' and 'sdk/' to any folder:
     ./bin/loom --help
     ./bin/loom new my_app
     cd my_app && ../bin/loom build

GETTING STARTED:
----------------
1. Create a new native workspace:
     loom new my_app
     cd my_app

2. Build and run:
     loom build --release
     loom run

3. Generate dependency graph:
     loom graph --view calls --format mermaid

4. Install the VS Code extension:
     code --install-extension /usr/local/share/loom/vscode/Loom-1.1.0.vsix

UNINSTALL:
----------
   sudo ./uninstall.sh
================================================================================
EOF

    echo "  -> Creating compressed DMG image..."
    rm -f "$NATIVE_DMG_PATH"
    hdiutil create -size 300m \
                   -volname "Loom Native (Apple Silicon)" \
                   -srcfolder "$NATIVE_DMG_STAGING" \
                   -ov \
                   -format UDZO \
                   "$NATIVE_DMG_PATH"

    echo "  [OK] Successfully created: $NATIVE_DMG_PATH"
    ls -lh "$NATIVE_DMG_PATH"
}

# ==============================================================================
# 2. RP2040 Embedded Toolchain: Loom-${VERSION}-macos-arm64-rp2040.dmg
# ==============================================================================
build_rp2040_dmg() {
    echo ""
    echo "===================================================================="
    echo " Building Toolchain 2: RP2040 Embedded Toolchain (ARM Cortex-M0+)   "
    echo " Package: Loom-${VERSION}-macos-arm64-rp2040.dmg                    "
    echo "===================================================================="
    local RP2040_PKG_ROOT="$BUILD_DIR/pkg_root_rp2040"
    local RP2040_DMG_STAGING="$BUILD_DIR/dmg_staging_rp2040"
    local RP2040_DMG_NAME="Loom-${VERSION}-macos-arm64-rp2040.dmg"
    local RP2040_DMG_PATH="$REPO_ROOT/$RP2040_DMG_NAME"

    rm -rf "$RP2040_PKG_ROOT" "$RP2040_DMG_STAGING"
    mkdir -p "$RP2040_PKG_ROOT/usr/local/bin"
    mkdir -p "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/targets/rp2040/bsp"
    mkdir -p "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/std"
    mkdir -p "$RP2040_PKG_ROOT/usr/local/share/loom/vscode"

    # Binaries
    cp -f "$REPO_ROOT/bin/loom" "$RP2040_PKG_ROOT/usr/local/bin/loom"
    cp -f "$REPO_ROOT/bin/loomc" "$RP2040_PKG_ROOT/usr/local/bin/loomc"
    chmod 755 "$RP2040_PKG_ROOT/usr/local/bin/loom" "$RP2040_PKG_ROOT/usr/local/bin/loomc"

    # RP2040 Bootloader & Linker script
    cp -f "$REPO_ROOT/sdk/targets/rp2040/boot.s" "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/targets/rp2040/boot.s"
    cp -f "$REPO_ROOT/sdk/targets/rp2040/linker.ld" "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/targets/rp2040/linker.ld"

    # RP2040 BSP (all 16 peripherals including clock.loom)
    cp -Rf "$REPO_ROOT/sdk/targets/rp2040/bsp/"* "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/targets/rp2040/bsp/"

    # Standard library
    cp -Rf "$REPO_ROOT/sdk/std/"* "$RP2040_PKG_ROOT/usr/local/share/loom/sdk/std/"

    # VS Code extension
    cp -f "$REPO_ROOT/vscode-loom/Loom-1.1.0.vsix" "$RP2040_PKG_ROOT/usr/local/share/loom/vscode/Loom-1.1.0.vsix"

    echo "  -> Building installer package (.pkg)..."
    mkdir -p "$RP2040_DMG_STAGING"
    pkgbuild --root "$RP2040_PKG_ROOT" \
             --identifier "org.loom.lang.rp2040" \
             --version "$VERSION" \
             --install-location "/" \
             "$RP2040_DMG_STAGING/Install Loom RP2040 (Apple Silicon).pkg"

    echo "  -> Assembling DMG staging directory..."
    mkdir -p "$RP2040_DMG_STAGING/bin"
    cp -f "$REPO_ROOT/bin/loom" "$RP2040_DMG_STAGING/bin/loom"
    cp -f "$REPO_ROOT/bin/loomc" "$RP2040_DMG_STAGING/bin/loomc"
    chmod 755 "$RP2040_DMG_STAGING/bin/loom" "$RP2040_DMG_STAGING/bin/loomc"

    mkdir -p "$RP2040_DMG_STAGING/sdk/targets/rp2040/bsp"
    cp -f "$REPO_ROOT/sdk/targets/rp2040/boot.s" "$RP2040_DMG_STAGING/sdk/targets/rp2040/boot.s"
    cp -f "$REPO_ROOT/sdk/targets/rp2040/linker.ld" "$RP2040_DMG_STAGING/sdk/targets/rp2040/linker.ld"
    cp -Rf "$REPO_ROOT/sdk/targets/rp2040/bsp/"* "$RP2040_DMG_STAGING/sdk/targets/rp2040/bsp/"

    mkdir -p "$RP2040_DMG_STAGING/sdk/std"
    cp -Rf "$REPO_ROOT/sdk/std/"* "$RP2040_DMG_STAGING/sdk/std/"

    mkdir -p "$RP2040_DMG_STAGING/vscode"
    cp -f "$REPO_ROOT/vscode-loom/Loom-1.1.0.vsix" "$RP2040_DMG_STAGING/vscode/Loom-1.1.0.vsix"

    cat << 'EOF' > "$RP2040_DMG_STAGING/install.sh"
#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Installing Loom RP2040 Embedded Toolchain (ARM Cortex-M0+)..."
sudo mkdir -p /usr/local/bin
sudo mkdir -p /usr/local/share/loom/sdk/targets/rp2040/bsp
sudo mkdir -p /usr/local/share/loom/sdk/std
sudo mkdir -p /usr/local/share/loom/vscode

echo "  -> Installing compiler (loomc) and build driver (loom) to /usr/local/bin"
sudo cp -f "$DIR/bin/loom" /usr/local/bin/loom
sudo cp -f "$DIR/bin/loomc" /usr/local/bin/loomc
sudo chmod 755 /usr/local/bin/loom /usr/local/bin/loomc

echo "  -> Installing RP2040 BSP and bootloader to /usr/local/share/loom/sdk/targets/rp2040"
sudo cp -Rf "$DIR/sdk/targets/rp2040/"* /usr/local/share/loom/sdk/targets/rp2040/

echo "  -> Installing standard library to /usr/local/share/loom/sdk/std"
sudo cp -Rf "$DIR/sdk/std/"* /usr/local/share/loom/sdk/std/

echo "  -> Copying VS Code extension to /usr/local/share/loom/vscode"
sudo cp -f "$DIR/vscode/Loom-1.1.0.vsix" /usr/local/share/loom/vscode/

echo ""
echo "Loom RP2040 Embedded Toolchain successfully installed!"
echo "Verify installation:"
echo "  loom --version"
echo "  loomc --version"
echo ""
echo "Create a new Raspberry Pi Pico firmware workspace:"
echo "  loom new my_pico --rp2040"
echo "  cd my_pico"
echo "  loom build"
echo ""
echo "To install the VS Code extension:"
echo "  code --install-extension /usr/local/share/loom/vscode/Loom-1.1.0.vsix"
EOF
    chmod 755 "$RP2040_DMG_STAGING/install.sh"

    cat << 'EOF' > "$RP2040_DMG_STAGING/uninstall.sh"
#!/usr/bin/env bash
set -euo pipefail
echo "Uninstalling Loom RP2040 Embedded Toolchain..."
sudo rm -rf /usr/local/share/loom/sdk/targets/rp2040
echo "Loom RP2040 Embedded Toolchain components uninstalled."
EOF
    chmod 755 "$RP2040_DMG_STAGING/uninstall.sh"

    cat << 'EOF' > "$RP2040_DMG_STAGING/README.txt"
================================================================================
Loom Programming Language - RP2040 Embedded Toolchain (ARM Cortex-M0+)
macOS Apple Silicon Host Release - Version 1.1.0
================================================================================

This package contains the complete turnkey bare-metal embedded toolchain for the
Raspberry Pi RP2040 microcontroller (Raspberry Pi Pico), running on macOS
Apple Silicon.

CONTENTS:
---------
- Compiler:      /usr/local/bin/loomc (LLVM ARM Cortex-M0+ thumbv6m backend)
- Build Driver:  /usr/local/bin/loom (UF2 packager, Wokwi simulator runner)
- Boot & Linker: /usr/local/share/loom/sdk/targets/rp2040/ (boot.s, linker.ld)
- RP2040 HAL:    /usr/local/share/loom/sdk/targets/rp2040/bsp/ (16 peripheral drivers)
                 - Clock: Full XOSC, ROSC, PLLs, Glitchless Mux, FC0, Resus
                 - System, GPIO, Time, UART, SPI, I2C, ADC, PWM, DMA,
                   Watchdog, Multicore, PIO, RTC, Interp, RP2040 umbrella
- Standard Lib:  /usr/local/share/loom/sdk/std/ (math.loom)
- VS Code VSIX:  /usr/local/share/loom/vscode/Loom-1.1.0.vsix

INSTALLATION OPTIONS:
---------------------
1. GUI Package Installer (Recommended):
   Double-click "Install Loom RP2040 (Apple Silicon).pkg" and follow the prompts.

2. Terminal Script:
   Open Terminal in this mounted disk image and run:
     sudo ./install.sh

3. Portable / Zero-Install Usage:
   You can run directly from this disk image or copy 'bin/' and 'sdk/' to any folder:
     ./bin/loom new my_pico --rp2040
     cd my_pico && ../bin/loom build

CREATING A PICO FIRMWARE PROJECT:
---------------------------------
1. Scaffold firmware workspace:
     loom new my_pico --rp2040
     cd my_pico

2. Compile to .elf, .bin, and drag-and-drop .uf2:
     loom build --release

3. Flash to hardware:
     Hold the BOOTSEL button while connecting USB, then copy target/my_pico.uf2
     to the mounted RPI-RP2 drive volume.

4. Install the VS Code extension:
     code --install-extension /usr/local/share/loom/vscode/Loom-1.1.0.vsix

UNINSTALL:
----------
   sudo ./uninstall.sh
================================================================================
EOF

    echo "  -> Creating compressed DMG image..."
    rm -f "$RP2040_DMG_PATH"
    hdiutil create -size 300m \
                   -volname "Loom RP2040 (Apple Silicon)" \
                   -srcfolder "$RP2040_DMG_STAGING" \
                   -ov \
                   -format UDZO \
                   "$RP2040_DMG_PATH"

    echo "  [OK] Successfully created: $RP2040_DMG_PATH"
    ls -lh "$RP2040_DMG_PATH"
}

case "$TARGET_FILTER" in
    native|desktop)
        build_native_dmg
        ;;
    rp2040|embedded)
        build_rp2040_dmg
        ;;
    all|*)
        build_native_dmg
        build_rp2040_dmg
        ;;
esac

echo ""
echo "=========================================================="
echo " All requested toolchain DMG packages built successfully! "
echo "=========================================================="
ls -lh "$REPO_ROOT"/*.dmg
