# Console table shared by tools/steamos/build_in_sdk.sh and
# tools/macos/build_macos_app.sh: which console a ROM is, and how that
# console's recompiler is called. Adding a console is one entry in each
# function. Written for the bash 3.2 that ships with macOS (no associative
# arrays).

# console_for_rom FILE: the console id for a ROM file name.
console_for_rom() {
    case "$(echo "$1" | tr '[:upper:]' '[:lower:]')" in
        *.gba) echo gba ;;
        *.sfc|*.smc) echo snes ;;
        *.nes) echo nes ;;
        *) echo gb ;;
    esac
}

# console_info ID: sets recompiler (Ninja target and executable name),
# name_option (how it takes the game id), runtime (the runtime folder inside a
# generated project) and takes_traces (1 if it accepts --trace).
console_info() {
    case "$1" in
        gb) recompiler=gbrecomp name_option=--output-prefix runtime=runtime takes_traces=0 ;;
        gba) recompiler=gbarecomp name_option=--name runtime=runtime-gba takes_traces=1 ;;
        snes) recompiler=snesrecomp name_option=--name runtime=runtime-snes takes_traces=1 ;;
        nes) recompiler=nesrecomp name_option=--name runtime=runtime-nes takes_traces=1 ;;
        *) echo "unknown console '$1' (use gb, gba, snes or nes)" >&2; return 2 ;;
    esac
}
