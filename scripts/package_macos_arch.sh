#!/usr/bin/env bash
set -euo pipefail

APP_NAME="FaeryTale"
BINARY_NAME="faerytale"
ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
DIST_DIR="${DIST_DIR:-"$ROOT/dist"}"
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"

usage() {
  cat <<'EOF'
Build and package FaeryTale.app for one macOS architecture.

Usage:
  scripts/package_macos_arch.sh x86_64
  scripts/package_macos_arch.sh arm64

Optional environment variables:
  MX                    Path to the mx compiler
  DIST_DIR              Output directory (default: ./dist)
  X86_64_BREW_PREFIX    Intel Homebrew prefix (default: /usr/local)
  ARM64_BREW_PREFIX     Apple Silicon Homebrew prefix (default: /opt/homebrew)
  CODESIGN_IDENTITY     Signing identity (default: - for ad-hoc signing)
EOF
}

case "${1:-}" in
  x86_64)
    ARCH="x86_64"
    TARGET="x86_64-darwin"
    BREW_PREFIX="${X86_64_BREW_PREFIX:-/usr/local}"
    ;;
  arm64)
    ARCH="arm64"
    TARGET="aarch64-darwin"
    BREW_PREFIX="${ARM64_BREW_PREFIX:-/opt/homebrew}"
    ;;
  --help|-h)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 1
    ;;
esac

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
  lipo -archs "$file" 2>/dev/null | tr ' ' '\n' | grep -qx "$ARCH"
}

is_system_dependency() {
  case "$1" in
    /System/*|/usr/lib/*) return 0 ;;
    *) return 1 ;;
  esac
}

resolve_dependency() {
  local dependency="$1"
  local relative=""
  local candidate=""
  local basename_only=""

  if [[ -f "$dependency" ]] && has_arch "$dependency"; then
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
    candidate="$BREW_PREFIX/$relative"
    if [[ -f "$candidate" ]] && has_arch "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  fi

  basename_only="$(basename "$dependency")"
  candidate="$(find -L "$BREW_PREFIX/opt" "$BREW_PREFIX/lib" \
    -name "$basename_only" -type f -print -quit 2>/dev/null || true)"
  if [[ -n "$candidate" ]] && has_arch "$candidate"; then
    printf '%s\n' "$candidate"
    return 0
  fi

  echo "error: cannot find $ARCH dependency $dependency under $BREW_PREFIX" >&2
  return 1
}

list_dependencies() {
  otool -L "$1" | tail -n +2 | awk '{ print $1 }'
}

bundle_dependency() {
  local dependency="$1"
  local frameworks_dir="$2"
  local source=""
  local destination=""
  local child=""

  is_system_dependency "$dependency" && return 0

  source="$(resolve_dependency "$dependency")"
  destination="$frameworks_dir/$(basename "$source")"
  if [[ -f "$destination" ]]; then
    return 0
  fi

  cp "$source" "$destination"
  chmod u+w "$destination"

  while IFS= read -r child; do
    if [[ "$(basename "$child")" != "$(basename "$source")" ]]; then
      bundle_dependency "$child" "$frameworks_dir"
    fi
  done < <(list_dependencies "$source")
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

for dylib in \
  "$BREW_PREFIX/opt/sdl2/lib/libSDL2-2.0.0.dylib" \
  "$BREW_PREFIX/opt/sdl2_ttf/lib/libSDL2_ttf-2.0.0.dylib"; do
  if [[ ! -f "$dylib" ]] || ! has_arch "$dylib"; then
    echo "error: missing $ARCH Homebrew dependency: $dylib" >&2
    echo "Install sdl2 and sdl2_ttf for $ARCH, or set the matching Homebrew prefix." >&2
    exit 1
  fi
done

WRAPPER="$WORK/clang-$ARCH"
cat > "$WRAPPER" <<EOF
#!/bin/sh
exec /usr/bin/clang -arch "$ARCH" -isysroot "$SDK_PATH" \
  -I"$BREW_PREFIX/opt/sdl2/include" \
  -I"$BREW_PREFIX/opt/sdl2/include/SDL2" \
  -I"$BREW_PREFIX/opt/sdl2_ttf/include" \
  -I"$BREW_PREFIX/opt/sdl2_ttf/include/SDL2" \
  -L"$BREW_PREFIX/opt/sdl2/lib" \
  -L"$BREW_PREFIX/opt/sdl2_ttf/lib" \
  "\$@"
EOF
chmod +x "$WRAPPER"

echo "Building $APP_NAME for $ARCH..."
rm -rf "$ROOT/.mx"
(
  cd "$ROOT"
  "$MX" build --release --target "$TARGET" --cc "$WRAPPER"
)

APP_DIR="$DIST_DIR/$APP_NAME-$ARCH.app"
CONTENTS_DIR="$APP_DIR/Contents"
MACOS_DIR="$CONTENTS_DIR/MacOS"
FRAMEWORKS_DIR="$CONTENTS_DIR/Frameworks"
RESOURCES_DIR="$CONTENTS_DIR/Resources"
ZIP_PATH="$DIST_DIR/$APP_NAME-macos-$ARCH.zip"

rm -rf "$APP_DIR" "$ZIP_PATH"
mkdir -p "$MACOS_DIR" "$FRAMEWORKS_DIR" "$RESOURCES_DIR"
cp "$ROOT/.mx/bin/$BINARY_NAME" "$MACOS_DIR/$BINARY_NAME-bin"
chmod +x "$MACOS_DIR/$BINARY_NAME-bin"

while IFS= read -r dependency; do
  bundle_dependency "$dependency" "$FRAMEWORKS_DIR"
done < <(list_dependencies "$MACOS_DIR/$BINARY_NAME-bin")

rewrite_binary_dependencies "$MACOS_DIR/$BINARY_NAME-bin"
while IFS= read -r library; do
  rewrite_library_dependencies "$library"
  codesign --force --sign "$CODESIGN_IDENTITY" "$library"
done < <(find "$FRAMEWORKS_DIR" -maxdepth 1 -type f -print)
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
  <string>com.faerytale.mx.$ARCH</string>
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
