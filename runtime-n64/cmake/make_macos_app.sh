#!/bin/bash
# Compiles a generated Native Console project into a macOS app (<id>.app).
#
# Every generated project carries this script in its runtime snapshot, so a
# project recompiled on another computer (a hub "macOS kit" export) can be
# finished on any Mac. Needs the Xcode command line tools, CMake and Ninja.
# The app is universal (Apple Silicon and Intel), needs macOS 11 or newer, and
# carries SDL2.framework, so it runs on Macs without Homebrew.
#
# Usage: make_macos_app.sh --project DIR --name ID [--title NAME]
#                          [--out DIR] [--build DIR] [--zip]
#
# runtime/cmake and runtime-gba/cmake hold identical copies of this file.
# Written for the bash 3.2 that ships with macOS.
set -euo pipefail

SDL2_VERSION=2.32.10
SDL2_DMG_SHA256=4a7ac31640d70214e848f994be8a12849c0f97918a7e6c2e27a40036166d1a7f

project="" name="" title="" out="" build="" zip=0
while [ $# -gt 0 ]; do
    case "$1" in
        --project) project="$2"; shift 2 ;;
        --name) name="$2"; shift 2 ;;
        --title) title="$2"; shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --build) build="$2"; shift 2 ;;
        --zip) zip=1; shift ;;
        *) echo "make_macos_app.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$project" ] && [ -n "$name" ] || { echo "make_macos_app.sh: --project and --name are required" >&2; exit 2; }
[ -f "$project/CMakeLists.txt" ] || { echo "make_macos_app.sh: no CMakeLists.txt in $project" >&2; exit 2; }
[ "$(uname -s)" = Darwin ] || { echo "make_macos_app.sh: run this on macOS" >&2; exit 2; }
project="$(cd "$project" && pwd)"
[ -n "$title" ] || title="$name"
[ -n "$build" ] || build="$project/build"
[ -n "$out" ] || out="$(pwd)"
mkdir -p "$out" && out="$(cd "$out" && pwd)"

missing=""
xcode-select -p >/dev/null 2>&1 || missing="$missing
  xcode-select --install"
command -v cmake >/dev/null 2>&1 && command -v ninja >/dev/null 2>&1 || missing="$missing
  brew install cmake ninja      (Homebrew: https://brew.sh)"
if [ -n "$missing" ]; then
    echo "Building needs a few developer tools. Install them with:$missing" >&2
    echo "then run this again." >&2
    exit 1
fi

# The official SDL2 framework is universal and uses @rpath, so it can be
# copied into the app as it is.
cache="${XDG_CACHE_HOME:-$HOME/Library/Caches}/native-console"
framework="$cache/SDL2-$SDL2_VERSION/SDL2.framework"
if [ ! -d "$framework" ]; then
    echo "[macos] downloading SDL2 $SDL2_VERSION"
    mkdir -p "$cache"
    dmg="$cache/SDL2-$SDL2_VERSION.dmg"
    curl -fsSL -o "$dmg" "https://github.com/libsdl-org/SDL/releases/download/release-$SDL2_VERSION/SDL2-$SDL2_VERSION.dmg"
    echo "$SDL2_DMG_SHA256  $dmg" | shasum -a 256 -c - >/dev/null || { echo "SDL2 download checksum mismatch" >&2; exit 1; }
    mount="$(mktemp -d)"
    hdiutil attach -quiet -nobrowse -readonly -mountpoint "$mount" "$dmg"
    mkdir -p "$(dirname "$framework")"
    ditto "$mount/SDL2.framework" "$framework"
    hdiutil detach -quiet "$mount"
    rm -f "$dmg"
fi

echo "[macos] compiling $title (arm64 + x86_64)"
cmake -G Ninja -S "$project" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0 \
    -DSDL2_DIR="$framework/Resources/CMake" -DNATIVE_CONSOLE_MACOS_BUNDLE=ON \
    -DNATIVE_CONSOLE_APP_NAME="$title" -DGBRECOMP_GENERATED_COMPILE_JOBS="$(sysctl -n hw.ncpu)" >/dev/null
ninja -C "$build"

app="$build/$name.app"
exe="$app/Contents/MacOS/$name"
echo "[macos] checking the app"
lipo "$exe" -verify_arch arm64 x86_64
otool -L "$exe" | grep -q "@rpath/SDL2.framework" || { echo "the app does not load the bundled SDL2" >&2; exit 1; }
[ -d "$app/Contents/Frameworks/SDL2.framework" ] || { echo "SDL2.framework missing from the app" >&2; exit 1; }
codesign --verify --deep --strict "$app"

rm -rf "$out/$name.app"
ditto "$app" "$out/$name.app"
if [ "$zip" = 1 ]; then
    rm -f "$out/$name-macos.zip"
    ditto -c -k --keepParent "$out/$name.app" "$out/$name-macos.zip"
fi
echo "[macos] done: $out/$name.app ($(du -sh "$out/$name.app" | cut -f1), $(lipo -archs "$exe"))"
