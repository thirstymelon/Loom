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
SSD1306_VERSION="${SSD1306_VERSION:-$VERSION}"
MPU6050_VERSION="${MPU6050_VERSION:-$VERSION}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
mkdir -p "$OUTPUT_DIR"

package_library() {
  local name="$1" version="$2"
  local root="$STAGE/$name"
  mkdir -p "$root"
  cp -R "$REPO_ROOT/libraries/$name/"* "$root/"
  tar -czf "$OUTPUT_DIR/loom-library-$name-$version.tar.gz" -C "$STAGE" "$name"
}

bundle_macos_dependencies() {
  local binary="$1" libdir="$2" llvm_prefix
  llvm_prefix="$(brew --prefix llvm)"
  local queue=("$binary") seen=() cursor=0 current dependency base source copied search
  mkdir -p "$libdir"
  while (( cursor < ${#queue[@]} )); do
    current="${queue[$cursor]}"
    cursor=$((cursor + 1))
    case " ${seen[*]} " in *" $current "*) continue ;; esac
    seen+=("$current")
    while IFS= read -r dependency; do
      [[ -z "$dependency" ]] && continue
      source="$dependency"
      if [[ "$dependency" == @rpath/* ]]; then
        base="$(basename "$dependency")"
        source=""
        for search in "$llvm_prefix/lib" /opt/homebrew/opt/z3/lib /opt/homebrew/opt/zstd/lib; do
          if [[ -f "$search/$base" ]]; then source="$search/$base"; break; fi
        done
      fi
      [[ -f "$source" ]] || { echo "Cannot bundle runtime dependency: $dependency" >&2; exit 1; }
      base="$(basename "$source")"
      copied="$libdir/$base"
      if [[ ! -f "$copied" ]]; then cp -L "$source" "$copied"; fi
      install_name_tool -change "$dependency" "@rpath/$base" "$current"
      if [[ "$copied" == *.dylib ]]; then install_name_tool -id "@rpath/$base" "$copied"; fi
      queue+=("$copied")
    done < <(otool -L "$current" | awk 'NR > 1 && ($1 ~ /^@rpath\// || $1 ~ /^\//) && $1 !~ /^\/usr\/lib\// && $1 !~ /^\/System\// { print $1 }')
  done
  install_name_tool -add_rpath '@loader_path/../lib' "$binary"
}

package_toolchain() {
  local name="$1" kind="$2"
  local root="$STAGE/$name"
  mkdir -p "$root/bin" "$root/sdk/std"
  cp "$BUILD_DIR/loomc-$kind" "$root/bin/loomc"
  chmod 755 "$root/bin/loomc"
  bundle_macos_dependencies "$root/bin/loomc" "$root/lib"
  cp -R "$REPO_ROOT/sdk/std/"* "$root/sdk/std/"
  if [[ "$name" == "rp2040" ]]; then
    cp "$(brew --prefix llvm)/bin/llvm-objcopy" "$root/bin/llvm-objcopy"
    chmod 755 "$root/bin/llvm-objcopy"
    bundle_macos_dependencies "$root/bin/llvm-objcopy" "$root/lib"
    mkdir -p "$root/sdk/targets/rp2040"
    cp -R "$REPO_ROOT/sdk/targets/rp2040/"* "$root/sdk/targets/rp2040/"
    cat > "$root/toolchain.toml" <<MANIFEST
name = "rp2040"
kind = "rp2040"
version = "$VERSION"
description = "Loom RP2040 Cortex-M0+ toolchain"
target = "thumbv6m-none-eabi"
cpu = "cortex-m0plus"
MANIFEST
  else
    cat > "$root/toolchain.toml" <<MANIFEST
name = "native"
kind = "native"
version = "$VERSION"
description = "Loom native toolchain for $PLATFORM"
MANIFEST
  fi
  tar -czf "$OUTPUT_DIR/loom-toolchain-$name-$PLATFORM-$VERSION.tar.gz" -C "$STAGE" "$name"
  rm -rf "$root"
}

package_library ssd1306 "$SSD1306_VERSION"
package_library mpu6050 "$MPU6050_VERSION"
package_toolchain native native
package_toolchain rp2040 rp2040

release_tag="v$VERSION"
catalog="$OUTPUT_DIR/loom-catalog.tsv"
: > "$catalog"
for name in ssd1306 mpu6050; do
  if [[ "$name" == "ssd1306" ]]; then version="$SSD1306_VERSION"; else version="$MPU6050_VERSION"; fi
  file="$OUTPUT_DIR/loom-library-$name-$version.tar.gz"
  sha="$(shasum -a 256 "$file" | awk '{print $1}')"
  printf 'library\t%s\t%s\thttps://github.com/thirstymelon/Loom/releases/download/%s/%s\t%s\n' \
    "$name" "$version" "$release_tag" "$(basename "$file")" "$sha" >> "$catalog"
done
for name in native rp2040; do
  file="$OUTPUT_DIR/loom-toolchain-$name-$PLATFORM-$VERSION.tar.gz"
  sha="$(shasum -a 256 "$file" | awk '{print $1}')"
  printf 'toolchain\t%s\t%s\t%s\thttps://github.com/thirstymelon/Loom/releases/download/%s/%s\t%s\n' \
    "$name" "$VERSION" "$PLATFORM" "$release_tag" "$(basename "$file")" "$sha" >> "$catalog"
done
