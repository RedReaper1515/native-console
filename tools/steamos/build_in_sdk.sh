#!/bin/bash
# Builds one recompiled game for Linux / SteamOS inside Valve's Steam Runtime
# 3 "sniper" SDK container. Run through tools/steamos/build_steamos.ps1 (or
# the docker command in LINUX.md); it expects these mounts:
#
#   /src   native-console checkout (read-only is fine)
#   /work  persistent build cache (a named Docker volume)
#   /out   destination for the packaged game
#
# Usage: build_in_sdk.sh --rom FILE --name ID [--console gb|gba|snes]
#                        [--trace FILE]... [-- extra recompiler arguments]
#
# The console follows the ROM's extension unless given (tools/consoles.sh).
#
# Output: /out/ID/ID (the executable, ROM embedded) and
#         /out/ID-steamos-x86_64.tar.gz (the same folder, executable bit kept).
set -euo pipefail

rom="" name="" console="" traces=() extra=()
while [ $# -gt 0 ]; do
    case "$1" in
        --rom) rom="$2"; shift 2 ;;
        --name) name="$2"; shift 2 ;;
        --console) console="$2"; shift 2 ;;
        --trace) traces+=("$2"); shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *) echo "build_in_sdk.sh: unknown argument $1" >&2; exit 2 ;;
    esac
done
[ -n "$rom" ] && [ -n "$name" ] || { echo "build_in_sdk.sh: --rom and --name are required" >&2; exit 2; }
[ -f "$rom" ] || { echo "build_in_sdk.sh: ROM not found: $rom" >&2; exit 2; }
case "$name" in *[!A-Za-z0-9_]*|"") echo "build_in_sdk.sh: --name must be letters, digits and underscores" >&2; exit 2 ;; esac
. /src/tools/consoles.sh
[ -n "$console" ] || console=$(console_for_rom "$rom")
console_info "$console" || exit 2
[ ${#traces[@]} -eq 0 ] || [ "$takes_traces" = 1 ] || { echo "build_in_sdk.sh: --trace does not apply to $console games" >&2; exit 2; }

# gcc 14 ships in the SDK and links libstdc++ statically, so the executables
# need only glibc >= 2.29 and SDL2 at run time.
export CC=gcc-14 CXX=g++-14
jobs=$(nproc)

echo "[steamos] syncing sources"
mkdir -p /work/nc
rsync -a --delete --exclude=/build --exclude='/build-*' --exclude='/build_*' --exclude=/output \
    --exclude=.git/ --exclude=__pycache__ /src/ /work/nc/

echo "[steamos] building $recompiler"
cmake -G Ninja -S /work/nc -B /work/tools -DCMAKE_BUILD_TYPE=Release -DBUILD_HUB=OFF -DBUILD_TESTS=OFF >/dev/null
ninja -C /work/tools "$recompiler"

project="/work/games/$name"
echo "[steamos] recompiling $(basename "$rom") ($console)"
trace_args=()
for t in ${traces[@]+"${traces[@]}"}; do trace_args+=(--trace "$t"); done
"/work/tools/bin/$recompiler" "$rom" -o "$project" "$name_option" "$name" ${trace_args[@]+"${trace_args[@]}"} ${extra[@]+"${extra[@]}"}

echo "[steamos] compiling"
cmake -G Ninja -S "$project" -B "$project/build" -DCMAKE_BUILD_TYPE=Release \
    -DGBRECOMP_GENERATED_COMPILE_JOBS="$jobs" >/dev/null
ninja -C "$project/build"

echo "[steamos] packaging"
stage="/work/stage/$name"
rm -rf "$stage" && mkdir -p "$stage"
install -m 0755 "$project/build/$name" "$stage/$name"
tar -C /work/stage -czf "/work/stage/$name-steamos-x86_64.tar.gz" "$name"
rm -rf "/out/$name" && mkdir -p "/out/$name"
cp "$stage/$name" "/out/$name/$name"
cp "/work/stage/$name-steamos-x86_64.tar.gz" /out/

glibc=$(objdump -T "$stage/$name" | grep -oE 'GLIBC_[0-9.]+' | sort -uV | tail -1)
libs=$(readelf -d "$stage/$name" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p' | tr '\n' ' ')
echo "[steamos] done: $name ($(du -h "$stage/$name" | cut -f1)), needs $glibc; links $libs"
