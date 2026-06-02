#!/usr/bin/env bash
set -euo pipefail

APP_NAME="FaeryTale"
BINARY_NAME="faerytale"
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIST_DIR="${DIST_DIR:-"$ROOT/dist"}"
X86_64_BREW_PREFIX="${X86_64_BREW_PREFIX:-/usr/local}"
ARM64_BREW_PREFIX="${ARM64_BREW_PREFIX:-/opt/homebrew}"
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"

usage() {
  cat <<'EOF'
Build and package a universal macOS FaeryTale.app for x86_64 and arm64.

Usage:
  scripts/package_macos_universal.sh

Optional environment variables:
  MX                    Path to the mx compiler
  DIST_DIR              Output directory (default: ./dist)
  X86_64_BREW_PREFIX    Intel Homebrew prefix (default: /usr/local)
  ARM64_BREW_PREFIX     Apple Silicon Homebrew prefix (default: /opt/homebrew)
  CODESIGN_IDENTITY     Signing identity (default: - for ad-hoc signing)

Both Homebrew prefixes must contain sdl2 and sdl2_ttf built for their
corresponding architecture.
EOF
}

if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
  usage
  exit 0
fi

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "error: this packager must run on macOS" >&2
  exit 1
fi

for tool in clang codesign ditto install_name_tool lipo otool xcrun; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: required tool is missing: $tool" >&2
    exit 1
  fi
done

MX="${MX:-"$ROOT/../mx/target/release/mx"}"
if [[ ! -x "$MX" ]]; then
  echo "error: mx compiler is not executable: $MX" >&2
  echo "Set MX to the installed mx executable or build ../mx first." >&2
  exit 1
fi

SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/faerytale-package.XXXXXX")"
MX_BACKUP="$WORK/mx-backup"
HAD_MX=0

restore_developer_build() {
  rm -rf "$ROOT/.mx"
  if [[ "$HAD_MX" == "1" ]]; then
    cp -R "$MX_BACKUP" "$ROOT/.mx"
  fi
  rm -rf "$WORK"
}
trap restore_developer_build EXIT

if [[ -d "$ROOT/.mx" ]]; then
  cp -R "$ROOT/.mx" "$MX_BACKUP"
  HAD_MX=1
fi

has_arch() {
  local file="$1"
  local arch="$2"
  lipo -archs "$file" 2>/dev/null | tr ' ' '\n' | grep -qx "$arch"
}

is_system_dependency() {
  case "$1" in
    /System/*|/usr/lib/*) return 0 ;;
    *) return 1 ;;
  esac
}

resolve_dependency() {
  local dependency="$1"
  local brew_prefix="$2"
  local arch="$3"
  local relative=""
  local candidate=""
  local basename_only=""

  if [[ -f "$dependency" ]] && has_arch "$dependency" "$arch"; then
    printf '%s\n' "$dependency"
    return 0
  fi

  case "$dependency" in
    /usr/local/*)
      relative="${dependency#/usr/local/}"
      ;;
    /opt/homebrew/*)
      relative="${dependency#/opt/homebrew/}"
      ;;
  esac
  if [[ -n "$relative" ]]; then
    candidate="$brew_prefix/$relative"
    if [[ -f "$candidate" ]] && has_arch "$candidate" "$arch"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi

  basename_only="$(basename "$dependency")"
  candidate="$(find -L "$brew_prefix/opt" "$brew_prefix/lib" \
    -name "$basename_only" -type f -print -quit 2>/dev/null || true)"
  if [[ -n "$candidate" ]] && has_arch "$candidate" "$arch"; then
    printf '%s\n' "$candidate"
    return 0
  fi

  echo "error: cannot find $arch dependency $dependency under $brew_prefix" >&2
  return 1
}

list_dependencies() {
  otool -L "$1" | tail -n +2 | awk '{ print $1 }'
}

bundle_dependency() {
  local dependency="$1"
  local brew_prefix="$2"
  local arch="$3"
  local frameworks_dir="$4"
  local source=""
  local destination=""
  local child=""

  is_system_dependency "$dependency" && return 0

  source="$(resolve_dependency "$dependency" "$brew_prefix" "$arch")"
  destination="$frameworks_dir/$(basename "$source")"
  if [[ -f "$destination" ]]; then
    return 0
  fi

  cp "$source" "$destination"
  chmod u+w "$destination"

  while IFS= read -r child; do
    if [[ "$(basename "$child")" != "$(basename "$source")" ]]; then
      bundle_dependency "$child" "$brew_prefix" "$arch" "$frameworks_dir"
    fi
  done < <(list_dependencies "$source")
}

write_compiler_wrapper() {
  local arch="$1"
  local brew_prefix="$2"
  local wrapper="$3"

  cat > "$wrapper" <<EOF
#!/bin/sh
exec /usr/bin/clang -arch "$arch" -isysroot "$SDK_PATH" \
  -I"$brew_prefix/opt/sdl2/include" \
  -I"$brew_prefix/opt/sdl2/include/SDL2" \
  -I"$brew_prefix/opt/sdl2_ttf/include" \
  -I"$brew_prefix/opt/sdl2_ttf/include/SDL2" \
  -L"$brew_prefix/opt/sdl2/lib" \
  -L"$brew_prefix/opt/sdl2_ttf/lib" \
  "\$@"
EOF
  chmod +x "$wrapper"
}

verify_architecture_dependencies() {
  local arch="$1"
  local brew_prefix="$2"

  for dylib in \
    "$brew_prefix/opt/sdl2/lib/libSDL2-2.0.0.dylib" \
    "$brew_prefix/opt/sdl2_ttf/lib/libSDL2_ttf-2.0.0.dylib"; do
    if [[ ! -f "$dylib" ]] || ! has_arch "$dylib" "$arch"; then
      echo "error: missing $arch Homebrew dependency: $dylib" >&2
      echo "Install sdl2 and sdl2_ttf for $arch, or set the matching Homebrew prefix." >&2
      exit 1
    fi
  done
}

build_architecture() {
  local arch="$1"
  local target="$2"
  local brew_prefix="$3"
  local arch_dir="$WORK/$arch"
  local wrapper="$WORK/clang-$arch"
  local dependency=""

  mkdir -p "$arch_dir/Frameworks"
  write_compiler_wrapper "$arch" "$brew_prefix" "$wrapper"

  echo "Building $APP_NAME for $arch..."
  rm -rf "$ROOT/.mx"
  (
    cd "$ROOT"
    "$MX" build --release --target "$target" --cc "$wrapper"
  )
  cp "$ROOT/.mx/bin/$BINARY_NAME" "$arch_dir/$BINARY_NAME"

  while IFS= read -r dependency; do
    bundle_dependency "$dependency" "$brew_prefix" "$arch" "$arch_dir/Frameworks"
  done < <(list_dependencies "$arch_dir/$BINARY_NAME")
}

rewrite_binary_dependencies() {
  local binary="$1"
  local dependency=""
  while IFS= read -r dependency; do
    if ! is_system_dependency "$dependency"; then
      install_name_tool -change "$dependency" \
        "@executable_path/../Frameworks/$(basename "$dependency")" "$binary"
    fi
  done < <(list_dependencies "$binary")
}

rewrite_library_dependencies() {
  local library="$1"
  local dependency=""
  install_name_tool -id "@loader_path/$(basename "$library")" "$library"
  while IFS= read -r dependency; do
    if ! is_system_dependency "$dependency" &&
       [[ "$(basename "$dependency")" != "$(basename "$library")" ]]; then
      install_name_tool -change "$dependency" \
        "@loader_path/$(basename "$dependency")" "$library"
    fi
  done < <(list_dependencies "$library")
}

verify_architecture_dependencies "x86_64" "$X86_64_BREW_PREFIX"
verify_architecture_dependencies "arm64" "$ARM64_BREW_PREFIX"
build_architecture "x86_64" "x86_64-darwin" "$X86_64_BREW_PREFIX"
build_architecture "arm64" "aarch64-darwin" "$ARM64_BREW_PREFIX"

for arch in x86_64 arm64; do
  rewrite_binary_dependencies "$WORK/$arch/$BINARY_NAME"
  while IFS= read -r library; do
    rewrite_library_dependencies "$library"
  done < <(find "$WORK/$arch/Frameworks" -maxdepth 1 -type f -print)
done

APP_DIR="$DIST_DIR/$APP_NAME.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
FRAMEWORKS_DIR="$CONTENTS_DIR/Frameworks"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
ZIP_PATH="$DIST_DIR/$APP_NAME-macos-universal.zip"

rm -rf "$APP_DIR" "$ZIP_PATH"
mkdir -p "$MACOS_DIR" "$FRAMEWORKS_DIR" "$RESOURCES_DIR"

lipo -create "$WORK/x86_64/$BINARY_NAME" "$WORK/arm64/$BINARY_NAME" \
  -output "$MACOS_DIR/$BINARY_NAME-bin"
chmod +x "$MACOS_DIR/$BINARY_NAME-bin"

while IFS= read -r library_name; do
    x86_library="$WORK/x86_64/Frameworks/$library_name"
    arm_library="$WORK/arm64/Frameworks/$library_name"
    if [[ ! -f "$x86_library" || ! -f "$arm_library" ]]; then
      echo "error: one architecture is missing a counterpart for $library_name" >&2
      exit 1
    fi
    lipo -create "$x86_library" "$arm_library" \
      -output "$FRAMEWORKS_DIR/$library_name"
    codesign --force --sign "$CODESIGN_IDENTITY" "$FRAMEWORKS_DIR/$library_name"
done < <(
  find "$WORK/x86_64/Frameworks" "$WORK/arm64/Frameworks" \
    -maxdepth 1 -type f -exec basename {} \; | sort -u
)

codesign --force --sign "$CODESIGN_IDENTITY" "$MACOS_DIR/$BINARY_NAME-bin"

cat > "$MACOS_DIR/$BINARY_NAME" <<'EOF'
#!/bin/sh
MACOS_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
CONTENTS_DIR="$(CDPATH= cd -- "$MACOS_DIR/.." && pwd)"
SUPPORT_DIR="$HOME/Library/Application Support/FaeryTale"

mkdir -p "$SUPPORT_DIR"
if [ -L "$SUPPORT_DIR/assets" ]; then
  ln -sfn "$CONTENTS_DIR/Resources/assets" "$SUPPORT_DIR/assets"
elif [ ! -e "$SUPPORT_DIR/assets" ]; then
  ln -s "$CONTENTS_DIR/Resources/assets" "$SUPPORT_DIR/assets"
fi

cd "$SUPPORT_DIR"
exec "$MACOS_DIR/faerytale-bin"
EOF
chmod +x "$MACOS_DIR/$BINARY_NAME"

cp -R "$ROOT/assets" "$RESOURCES_DIR/assets"

cat > "$CONTENTS_DIR/Info.plist" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleDisplayName</key>
  <string>$APP_NAME</string>
  <key>CFBundleExecutable</key>
  <string>$BINARY_NAME</string>
  <key>CFBundleIdentifier</key>
  <string>com.faerytale.mx</string>
  <key>CFBundleName</key>
  <string>$APP_NAME</string>
  <key>CFBundlePackageType</key>
  <string>APPL</string>
  <key>CFBundleShortVersionString</key>
  <string>0.1.0</string>
</dict>
</plist>
EOF

codesign --force --deep --sign "$CODESIGN_IDENTITY" "$APP_DIR"
ditto -c -k --sequesterRsrc --keepParent "$APP_DIR" "$ZIP_PATH"

echo "Created $APP_DIR"
echo "Created $ZIP_PATH"
