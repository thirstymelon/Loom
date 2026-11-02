#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$REPO_ROOT/.build"
DIST_DIR="$REPO_ROOT/dist"
PKG_ROOT="$BUILD_DIR/pkg_root"
DMG_STAGING="$BUILD_DIR/dmg_staging"
VERSION="1.0.0"
DMG_NAME="Loom-${VERSION}-macos-arm64.dmg"
DMG_PATH="$REPO_ROOT/$DMG_NAME"

echo "=== Building Loom binaries ==="
if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
    cmake -B "$BUILD_DIR" -S "$REPO_ROOT"
fi
cmake --build "$BUILD_DIR" -j8
mkdir -p "$REPO_ROOT/bin"
cp -f "$BUILD_DIR/loom" "$REPO_ROOT/bin/loom"
cp -f "$BUILD_DIR/loomc" "$REPO_ROOT/bin/loomc"
chmod 755 "$REPO_ROOT/bin/loom" "$REPO_ROOT/bin/loomc"

if [ -d "$REPO_ROOT/macos/LoomCompiler.app" ]; then
    echo "=== Updating LoomCompiler.app ==="
    mkdir -p "$REPO_ROOT/macos/LoomCompiler.app/Contents/MacOS"
    cp -f "$REPO_ROOT/bin/loomc" "$REPO_ROOT/macos/LoomCompiler.app/Contents/MacOS/LoomCompiler"
    chmod 755 "$REPO_ROOT/macos/LoomCompiler.app/Contents/MacOS/LoomCompiler"
fi

echo "=== Packaging VS Code extension ==="
if [ ! -f "$REPO_ROOT/vscode-loom/Loom-1.0.0.vsix" ]; then
    (cd "$REPO_ROOT/vscode-loom" && npx --yes @vscode/vsce package --allow-missing-repository --no-git-tag-version)
    cp -f "$REPO_ROOT/vscode-loom/loom-language-support-1.0.0.vsix" "$REPO_ROOT/vscode-loom/Loom-1.0.0.vsix"
fi

echo "=== Preparing pkg root payload ==="
rm -rf "$PKG_ROOT"
mkdir -p "$PKG_ROOT/usr/local/bin"
mkdir -p "$PKG_ROOT/usr/local/share/loom/sdk"
mkdir -p "$PKG_ROOT/usr/local/share/loom/vscode"

cp -f "$REPO_ROOT/bin/loom" "$PKG_ROOT/usr/local/bin/loom"
cp -f "$REPO_ROOT/bin/loomc" "$PKG_ROOT/usr/local/bin/loomc"
chmod 755 "$PKG_ROOT/usr/local/bin/loom" "$PKG_ROOT/usr/local/bin/loomc"

cp -Rf "$REPO_ROOT/sdk/"* "$PKG_ROOT/usr/local/share/loom/sdk/"
cp -f "$REPO_ROOT/vscode-loom/Loom-1.0.0.vsix" "$PKG_ROOT/usr/local/share/loom/vscode/Loom-1.0.0.vsix"

if [ -d "$REPO_ROOT/macos/LoomCompiler.app" ]; then
    mkdir -p "$PKG_ROOT/Applications"
    cp -Rf "$REPO_ROOT/macos/LoomCompiler.app" "$PKG_ROOT/Applications/LoomCompiler.app"
fi

echo "=== Building macOS installer package (.pkg) ==="
mkdir -p "$DMG_STAGING"
rm -rf "$DMG_STAGING"/*

pkgbuild --root "$PKG_ROOT" \
         --identifier "org.loom.lang" \
         --version "$VERSION" \
         --install-location "/" \
         "$DMG_STAGING/Install Loom.pkg"

echo "=== Preparing DMG staging directory ==="
if [ -d "$REPO_ROOT/macos/LoomCompiler.app" ]; then
    cp -Rf "$REPO_ROOT/macos/LoomCompiler.app" "$DMG_STAGING/LoomCompiler.app"
    ln -s /Applications "$DMG_STAGING/Applications"
fi

mkdir -p "$DMG_STAGING/bin"
cp -f "$REPO_ROOT/bin/loom" "$DMG_STAGING/bin/loom"
cp -f "$REPO_ROOT/bin/loomc" "$DMG_STAGING/bin/loomc"
chmod 755 "$DMG_STAGING/bin/loom" "$DMG_STAGING/bin/loomc"

mkdir -p "$DMG_STAGING/sdk"
cp -Rf "$REPO_ROOT/sdk/"* "$DMG_STAGING/sdk/"

mkdir -p "$DMG_STAGING/vscode"
cp -f "$REPO_ROOT/vscode-loom/Loom-1.0.0.vsix" "$DMG_STAGING/vscode/Loom-1.0.0.vsix"

cat << 'EOF' > "$DMG_STAGING/install.sh"
#!/usr/bin/env bash
set -euo pipefail
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "Installing Loom ${VERSION:-1.0.0} for macOS (Apple Silicon arm64)..."
sudo mkdir -p /usr/local/bin
sudo mkdir -p /usr/local/share/loom/sdk
sudo mkdir -p /usr/local/share/loom/vscode

echo "  -> Copying compiler and driver to /usr/local/bin"
sudo cp -f "$DIR/bin/loom" /usr/local/bin/loom
sudo cp -f "$DIR/bin/loomc" /usr/local/bin/loomc
sudo chmod 755 /usr/local/bin/loom /usr/local/bin/loomc

echo "  -> Copying SDK to /usr/local/share/loom/sdk"
sudo cp -Rf "$DIR/sdk/"* /usr/local/share/loom/sdk/

echo "  -> Copying VS Code extension to /usr/local/share/loom/vscode"
sudo cp -f "$DIR/vscode/Loom-1.0.0.vsix" /usr/local/share/loom/vscode/

if [ -d "$DIR/LoomCompiler.app" ]; then
    echo "  -> Installing LoomCompiler.app to /Applications"
    sudo cp -Rf "$DIR/LoomCompiler.app" /Applications/
    /System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "/Applications/LoomCompiler.app" 2>/dev/null || true
fi

echo ""
echo "Loom successfully installed!"
echo "Check installation with:"
echo "  loom --version"
echo "  loomc --version"
echo ""
echo "To install the VS Code extension, run:"
echo "  code --install-extension /usr/local/share/loom/vscode/Loom-1.0.0.vsix"
EOF
chmod 755 "$DMG_STAGING/install.sh"

cat << 'EOF' > "$DMG_STAGING/uninstall.sh"
#!/usr/bin/env bash
set -euo pipefail

echo "Uninstalling Loom from macOS..."
sudo rm -f /usr/local/bin/loom
sudo rm -f /usr/local/bin/loomc
sudo rm -rf /usr/local/share/loom
sudo rm -rf /Applications/LoomCompiler.app
echo "Loom has been completely removed."
EOF
chmod 755 "$DMG_STAGING/uninstall.sh"

cat << 'EOF' > "$DMG_STAGING/README.txt"
Loom Programming Language - macOS arm64 (Apple Silicon) Release
Version 1.0.0

INSTALLATION OPTIONS:
---------------------
1. GUI Package Installer (Recommended):
   Double-click "Install Loom.pkg" and follow the on-screen installer prompts.
   This will install:
     - /usr/local/bin/loom
     - /usr/local/bin/loomc
     - /usr/local/share/loom/sdk/ (RP2040 HAL, boot.s, linker.ld, stdlib)
     - /usr/local/share/loom/vscode/Loom-1.0.0.vsix

2. Terminal Script:
   Open Terminal in this disk image and run:
     sudo ./install.sh

VS CODE EXTENSION:
------------------
To install the VS Code extension for Loom syntax highlighting, snippets,
formatting, and real-time error checking:
   code --install-extension /usr/local/share/loom/vscode/Loom-1.0.0.vsix

VERIFY INSTALLATION:
--------------------
   loom --version
   loomc --version

UNINSTALL:
----------
   sudo ./uninstall.sh
EOF

echo "=== Creating disk image (.dmg) ==="
rm -f "$DMG_PATH"

hdiutil create -volname "Loom ${VERSION} (Apple Silicon)" \
               -srcfolder "$DMG_STAGING" \
               -ov \
               -format UDZO \
               "$DMG_PATH"

echo ""
echo "Successfully generated: $DMG_PATH"
ls -lh "$DMG_PATH"

