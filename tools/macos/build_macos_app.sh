#!/bin/bash
# Builds one recompiled game as a macOS application bundle (<id>.app).
#
# Run on a Mac with the Xcode command line tools, CMake and Ninja
# (xcode-select --install; brew install cmake ninja). The app is universal
# (Apple Silicon and Intel), needs macOS 11 or newer, and carries SDL2 inside,
# so it runs on Macs without Homebrew.
#
# Usage: tools/macos/build_macos_app.sh --rom FILE --name ID
#            [--console gb|gba|snes] [--title "Display Name"] [--trace FILE]...
#            [--out DIR] [-- extra recompiler arguments]
#
# The console follows the ROM's extension unless given (tools/consoles.sh).
#
# Output: DIR/ID.app and DIR/ID-macos.zip (default DIR: output/macos).
# Recompiles here, then finishes with the generated project's own
# runtime/cmake/make_macos_app.sh (the step a hub "macOS kit" runs on a Mac).
# Written for the bash 3.2 that ships with macOS.
set -euo pipefail

here="$(cd "$(dirname "$0")/../.." && pwd)"
rom="" name="" console="" title="" out="$here/output/macos"
traces=() extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) rom="$2"; shift 2 ;;
        --name) name="$2"; shift 2 ;;
        --console) console="$2"; shift 2 ;;
        --title) title="$2"; shift 2 ;;
        --trace) traces+=("$2"); shift 2 ;;
        --out) out="$2"; shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *) echo "build_macos_app.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$rom" ] && [ -n "$name" ] || { echo "build_macos_app.sh: --rom and --name are required" >&2; exit 2; }
[ -f "$rom" ] || { echo "build_macos_app.sh: ROM not found: $rom" >&2; exit 2; }
case "$name" in *[!A-Za-z0-9_]*|"") echo "build_macos_app.sh: --name must be letters, digits and underscores" >&2; exit 2 ;; esac
. "$here/tools/consoles.sh"
[ -n "$console" ] || console=$(console_for_rom "$rom")
console_info "$console" || exit 2
[ ${#traces[@]} -eq 0 ] || [ "$takes_traces" = 1 ] || { echo "build_macos_app.sh: --trace does not apply to $console games" >&2; exit 2; }
[ -n "$title" ] || title="$name"
[ "$(uname -s)" = Darwin ] || { echo "build_macos_app.sh: run this on macOS" >&2; exit 2; }
rom="$(cd "$(dirname "$rom")" && pwd)/$(basename "$rom")"
work="$here/build-macos"

echo "[macos] building $recompiler"
cmake -G Ninja -S "$here" -B "$work/tools" -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_HUB=OFF -DBUILD_TESTS=OFF >/dev/null
ninja -C "$work/tools" "$recompiler"

project="$work/games/$name"
echo "[macos] recompiling $(basename "$rom") ($console)"
trace_args=()
for t in ${traces[@]+"${traces[@]}"}; do trace_args+=(--trace "$t"); done
"$work/tools/bin/$recompiler" "$rom" -o "$project" "$name_option" "$name" ${trace_args[@]+"${trace_args[@]}"} ${extra[@]+"${extra[@]}"}

bash "$project/$runtime/cmake/make_macos_app.sh" --project "$project" --name "$name" --title "$title" --out "$out" --zip
