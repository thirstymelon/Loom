#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/.build"
VERSION="1.1.0"

TARGET_FILTER="${1:-all}"

echo "=========================================================="
echo " Loom Multi-Toolchain PKG Builder for macOS Apple Silicon "
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
# 1. Desktop Native Toolchain: Loom-${VERSION}-macos-arm64-native.pkg
# ==============================================================================
build_native_pkg() {
    echo ""
    echo "===================================================================="
    echo " Building Toolchain 1: macOS Apple Silicon Desktop Native           "
    echo " Package: Loom-${VERSION}-macos-arm64-native.pkg                    "
    echo "===================================================================="
    local NATIVE_PKG_ROOT="$BUILD_DIR/pkg_root_native"
    local NATIVE_SCRIPTS_DIR="$BUILD_DIR/pkg_scripts_native"
    local NATIVE_PKG_PATH="$REPO_ROOT/Loom-${VERSION}-macos-arm64-native.pkg"

    rm -rf "$NATIVE_PKG_ROOT" "$NATIVE_SCRIPTS_DIR"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/bin"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/bin"
    mkdir -p "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/sdk/std"
    mkdir -p "$NATIVE_SCRIPTS_DIR"

    # Universal build driver CLI
    cp -f "$REPO_ROOT/bin/loom" "$NATIVE_PKG_ROOT/usr/local/bin/loom"
    chmod 755 "$NATIVE_PKG_ROOT/usr/local/bin/loom"

    # Native Toolchain compiler (compiled with native-only enforcement)
    if [ -f "$BUILD_DIR/loomc-native" ]; then
        cp -f "$BUILD_DIR/loomc-native" "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/bin/loomc"
    else
        cp -f "$REPO_ROOT/bin/loomc" "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/bin/loomc"
    fi
    chmod 755 "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/bin/loomc"

    # Native Toolchain manifest
    cat << 'EOF' > "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/toolchain.toml"
name = "native"
kind = "native"
version = "1.1.0"
description = "Loom Native Desktop Toolchain (macOS Apple Silicon arm64)"
target = "arm64-apple-darwin"
EOF

    # Native Standard Library
    cp -Rf "$REPO_ROOT/sdk/std/"* "$NATIVE_PKG_ROOT/usr/local/share/loom/toolchains/native/sdk/std/"

    # Post-install script
    cat << 'EOF' > "$NATIVE_SCRIPTS_DIR/postinstall"
#!/bin/sh
set -e
chmod 755 /usr/local/bin/loom 2>/dev/null || true
if [ -d "/usr/local/share/loom/toolchains/native/bin" ]; then
    chmod 755 /usr/local/share/loom/toolchains/native/bin/loomc 2>/dev/null || true
fi
mkdir -p /usr/local/share/loom
if [ ! -f /usr/local/share/loom/default_toolchain ]; then
    echo "native" > /usr/local/share/loom/default_toolchain
fi
exit 0
EOF
    chmod 755 "$NATIVE_SCRIPTS_DIR/postinstall"

    echo "  -> Assembling macOS installer package (.pkg)..."
    pkgbuild --root "$NATIVE_PKG_ROOT" \
             --identifier "org.loom.lang.toolchain.native" \
             --version "$VERSION" \
             --install-location "/" \
             --scripts "$NATIVE_SCRIPTS_DIR" \
             "$NATIVE_PKG_PATH"

    echo "  -> Built: $NATIVE_PKG_PATH ($(du -h "$NATIVE_PKG_PATH" | cut -f1))"
}

# ==============================================================================
# 2. RP2040 Embedded Toolchain: Loom-${VERSION}-macos-arm64-rp2040.pkg
# ==============================================================================
build_rp2040_pkg() {
    echo ""
    echo "===================================================================="
    echo " Building Toolchain 2: RP2040 Embedded Bare-Metal (Cortex-M0+)      "
    echo " Package: Loom-${VERSION}-macos-arm64-rp2040.pkg                    "
    echo "===================================================================="
    local RP_PKG_ROOT="$BUILD_DIR/pkg_root_rp2040"
    local RP_SCRIPTS_DIR="$BUILD_DIR/pkg_scripts_rp2040"
    local RP_PKG_PATH="$REPO_ROOT/Loom-${VERSION}-macos-arm64-rp2040.pkg"

    rm -rf "$RP_PKG_ROOT" "$RP_SCRIPTS_DIR"
    mkdir -p "$RP_PKG_ROOT/usr/local/bin"
    mkdir -p "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/bin"
    mkdir -p "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/sdk/std"
    mkdir -p "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/sdk/targets/rp2040"
    mkdir -p "$RP_SCRIPTS_DIR"

    # Universal build driver CLI
    cp -f "$REPO_ROOT/bin/loom" "$RP_PKG_ROOT/usr/local/bin/loom"
    chmod 755 "$RP_PKG_ROOT/usr/local/bin/loom"

    # RP2040 Toolchain compiler (compiled with RP2040-only enforcement)
    if [ -f "$BUILD_DIR/loomc-rp2040" ]; then
        cp -f "$BUILD_DIR/loomc-rp2040" "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/bin/loomc"
    else
        cp -f "$REPO_ROOT/bin/loomc" "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/bin/loomc"
    fi
    chmod 755 "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/bin/loomc"

    # RP2040 Toolchain manifest
    cat << 'EOF' > "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/toolchain.toml"
name = "rp2040"
kind = "rp2040"
version = "1.1.0"
description = "Loom RP2040 Embedded Bare-Metal Toolchain (ARM Cortex-M0+)"
target = "thumbv6m-none-eabi"
cpu = "cortex-m0plus"
EOF

    # Embedded Standard Library & RP2040 Hardware SDK / BSP
    cp -Rf "$REPO_ROOT/sdk/std/"* "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/sdk/std/"
    cp -Rf "$REPO_ROOT/sdk/targets/rp2040/"* "$RP_PKG_ROOT/usr/local/share/loom/toolchains/rp2040/sdk/targets/rp2040/"

    # Post-install script
    cat << 'EOF' > "$RP_SCRIPTS_DIR/postinstall"
#!/bin/sh
set -e
chmod 755 /usr/local/bin/loom 2>/dev/null || true
if [ -d "/usr/local/share/loom/toolchains/rp2040/bin" ]; then
    chmod 755 /usr/local/share/loom/toolchains/rp2040/bin/loomc 2>/dev/null || true
fi
mkdir -p /usr/local/share/loom
if [ ! -f /usr/local/share/loom/default_toolchain ]; then
    echo "rp2040" > /usr/local/share/loom/default_toolchain
fi
exit 0
EOF
    chmod 755 "$RP_SCRIPTS_DIR/postinstall"

    echo "  -> Assembling macOS installer package (.pkg)..."
    pkgbuild --root "$RP_PKG_ROOT" \
             --identifier "org.loom.lang.toolchain.rp2040" \
             --version "$VERSION" \
             --install-location "/" \
             --scripts "$RP_SCRIPTS_DIR" \
             "$RP_PKG_PATH"

    echo "  -> Built: $RP_PKG_PATH ($(du -h "$RP_PKG_PATH" | cut -f1))"
}

case "$TARGET_FILTER" in
    native)
        build_native_pkg
        ;;
    rp2040)
        build_rp2040_pkg
        ;;
    all)
        build_native_pkg
        build_rp2040_pkg
        ;;
    *)
        echo "Error: Unknown target filter '$TARGET_FILTER'. Expected: native, rp2040, or all"
        exit 1
        ;;
esac

echo ""
echo "=========================================================="
echo " Packaging completed successfully!"
echo " Artifacts:"
ls -lh "$REPO_ROOT"/Loom-*.pkg
echo "=========================================================="

