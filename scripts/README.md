# Loom Toolchain Build & Packaging Scripts

This directory contains automation scripts for packaging the Loom native toolchain.

## Native Toolchain Packaging (`build_pkg.sh`)

Loom packages a standalone macOS Apple Silicon (`arm64`) installer package (`.pkg`):

- **Desktop Native Toolchain (`Loom-1.4.0-macos-arm64-native.pkg`)**:
  - Universal build driver CLI (`/usr/local/bin/loom`) and Language Server (`/usr/local/bin/loom-lsp`).
  - Native desktop compiler (`/usr/local/share/loom/toolchains/native/bin/loomc`) targeting host native architecture (`arm64-apple-darwin`).
  - Bundled Z3 and zstd runtime libraries, so the compiler does not need Homebrew's runtime paths.
  - Native runtime static library (`/usr/local/lib/libloom_rt.a`).
  - Toolchain manifest (`toolchain.toml`) with native desktop configuration.

### Usage

```bash
# Build the desktop native toolchain PKG
./scripts/build_pkg.sh
```

The generated `.pkg` file is written to the repository root.

## GitHub Release Packaging (`package_release.sh`)

Invoked by `.github/workflows/publish-release.yml` on release tags (`v*`). Builds:
- `loom-toolchain-native-macos-arm64-<version>.tar.gz` (toolchain package for `loom toolchain`)
- `loom-macos-arm64-v<version>.tar.gz` (standalone release archive)
- `loom-catalog.tsv` (SHA-256 verified registry index)
- `loom-language-support-<version>.vsix` (VS Code extension)
