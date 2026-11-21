#!/usr/bin/env bash
set -euo pipefail

if [[ $# -lt 3 ]]; then
  echo "usage: $0 <loom-version> <cmake-build-dir> <output-dir> [platform]" >&2
  exit 2
fi

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$1"
BUILD_DIR="$(cd "$2" && pwd)"
OUTPUT_DIR="$3"
PLATFORM="${4:-macos-arm64}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUTPUT_DIR"

bundle_macos_dependencies() {
  local binary="$1" libdir="$2" llvm_prefix
  llvm_prefix="$(brew --prefix llvm 2>/dev/null || echo "/opt/homebrew/opt/llvm")"
  local queue=("$binary") seen=() cursor=0 current dependency base source copied search
  mkdir -p "$libdir"
  while (( cursor < ${#queue[@]} )); do
    current="${queue[$cursor]}"
    cursor=$((cursor + 1))
    case " ${seen[*]-} " in *" $current "*) continue ;; esac
    seen+=("$current")
    while IFS= read -r dependency; do
      [[ -z "$dependency" ]] && continue
      source="$dependency"
      if [[ "$dependency" == @rpath/* ]]; then
        base="$(basename "$dependency")"
        source=""
        for search in "$llvm_prefix/lib" /opt/homebrew/opt/z3/lib /opt/homebrew/opt/zstd/lib /usr/local/lib; do
          if [[ -f "$search/$base" ]]; then source="$search/$base"; break; fi
        done
      fi
      [[ -f "$source" ]] || { echo "Cannot bundle runtime dependency: $dependency" >&2; exit 1; }
      base="$(basename "$source")"
      copied="$libdir/$base"
      if [[ ! -f "$copied" ]]; then
        cp -L "$source" "$copied"
        chmod 755 "$copied"
        if [[ "$copied" == *.dylib ]]; then
          install_name_tool -id "@rpath/$base" "$copied"
        fi
      fi
      install_name_tool -change "$dependency" "@rpath/$base" "$current"
      queue+=("$copied")
    done < <(otool -L "$current" | awk 'NR > 1 && ($1 ~ /^@rpath\// || $1 ~ /^\//) && $1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\// { print $1 }')
  done
  install_name_tool -add_rpath '@loader_path/../lib' "$binary" 2>/dev/null || true
}

package_native_toolchain() {
  local root="$STAGE/native"
  mkdir -p "$root/bin" "$root/lib"

  # Compiler
  if [[ -f "$BUILD_DIR/loomc" ]]; then
    cp "$BUILD_DIR/loomc" "$root/bin/loomc"
    chmod 755 "$root/bin/loomc"
    bundle_macos_dependencies "$root/bin/loomc" "$root/lib"
  fi

  # Build driver CLI
  if [[ -f "$BUILD_DIR/loom" ]]; then
    cp "$BUILD_DIR/loom" "$root/bin/loom"
    chmod 755 "$root/bin/loom"
  fi

  # Language server
  if [[ -f "$BUILD_DIR/loom-lsp" ]]; then
    cp "$BUILD_DIR/loom-lsp" "$root/bin/loom-lsp"
    chmod 755 "$root/bin/loom-lsp"
    bundle_macos_dependencies "$root/bin/loom-lsp" "$root/lib"
  fi

  # Runtime static library
  if [[ -f "$BUILD_DIR/libloom_rt.a" ]]; then
    cp "$BUILD_DIR/libloom_rt.a" "$root/lib/libloom_rt.a"
  fi

  cat > "$root/toolchain.toml" <<MANIFEST
name = "native"
kind = "native"
version = "$VERSION"
description = "Loom native toolchain for $PLATFORM"
target = "arm64-apple-darwin"
MANIFEST

  # Toolchain package for registry / loom CLI toolchain install
  tar -czf "$OUTPUT_DIR/loom-toolchain-native-$PLATFORM-$VERSION.tar.gz" -C "$STAGE" "native"

  # Full standalone archive
  tar -czf "$OUTPUT_DIR/loom-$PLATFORM-v$VERSION.tar.gz" -C "$STAGE/native" bin lib toolchain.toml
}

# 1. Package the native toolchain and standalone distribution
package_native_toolchain

# 2. Package VS Code extension if available
if [[ -d "$REPO_ROOT/vscode-loom" ]] && command -v npx >/dev/null 2>&1; then
  echo "Packaging VS Code extension..."
  (cd "$REPO_ROOT/vscode-loom" && npx --yes @vscode/vsce package --no-dependencies -o "$OUTPUT_DIR/loom-language-support-$VERSION.vsix" --allow-missing-repository --no-git-tag-version) || true
fi

# 3. Generate catalog TSV
release_tag="v$VERSION"
catalog="$OUTPUT_DIR/loom-catalog.tsv"
: > "$catalog"

file="$OUTPUT_DIR/loom-toolchain-native-$PLATFORM-$VERSION.tar.gz"
if [[ -f "$file" ]]; then
  sha="$(shasum -a 256 "$file" | awk '{print $1}')"
  printf 'toolchain\tnative\t%s\t%s\thttps://github.com/thirstymelon/Loom/releases/download/%s/%s\t%s\n' \
    "$VERSION" "$PLATFORM" "$release_tag" "$(basename "$file")" "$sha" >> "$catalog"
fi

echo "Release assets generated in $OUTPUT_DIR:"
ls -lh "$OUTPUT_DIR"
