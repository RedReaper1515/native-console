#!/usr/bin/env python3
"""Run Peter Lemon's (krom) RDP test ROMs and compare their framebuffers.

Each ROM under <data-dir>/krom/RDP has a reference screenshot next to it
(X.N64 and X.png). The runner draws for --frames frames and dumps the
framebuffer at VI_ORIGIN (n64_run --dump-fb) as the VI shows it: VI_WIDTH
pixels by the active lines of V_VIDEO times Y_SCALE.

The reference PNGs come in two kinds. Some are raw framebuffers. Others
(Peter Lemon's own 16BPP and 32BPP demos) are scan-outs: the 237 lines the
VI shows (V_VIDEO 0x025-0x1FF at Y_SCALE 1.0) stretched to 240 rows, with
reference row r showing framebuffer line floor((r + 0.5) * 237 / 240);
FillRectangle16BPP matches exactly that way and differs on six rows read
raw. Both readings are exact comparisons, and a ROM is scored by the one
with fewer differing pixels (the output says which). Pixels compare at the
framebuffer's precision: 5 bits per channel for 16-bit images, which the
PNGs expand as (c << 3) | (c >> 2).

The baseline (tests/n64/rdp_baseline.txt) records, for every ROM, the
number of pixels that differ. The check fails closed: a ROM missing from
the baseline or from the data, a runner error, a missing dump, or any
count that differs from the baseline (better or worse) is a failure.
After an intended change, rewrite the baseline with --update-baseline.

    compare_rdp_tests.py --runner build/bin/n64_run [--data-dir roms/n64]
        [--baseline tests/n64/rdp_baseline.txt] [--update-baseline]
        [--filter TEXT] [--frames N] [--jobs N] [--diff-dir DIR]
"""
import argparse
import os
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

try:
    from PIL import Image
except ImportError:  # fail closed: no comparison without the PNG reader
    print("compare_rdp_tests.py needs Pillow (python3 -m pip install pillow)")
    sys.exit(2)


BASELINE_HEADER = """\
# krom RDP test ROMs: pixels differing from the reference PNG, at the
# framebuffer's precision. Written by tools/n64/compare_rdp_tests.py
# --update-baseline; 0 is exact. Any change to a count fails the check.
#
# Known reasons for non-zero counts:
#  - 16BPP/ and 32BPP/ (Peter Lemon's demos): the references are emulator
#    scan-outs, not framebuffers. They stretch 237 VI lines to 240 with a
#    resampling that cannot be recovered exactly (edges of most shapes),
#    compute (1.0 - 0) * TEXEL0 as TEXEL0 * TEXEL0 (CombinerOverflow, a
#    hardware-checked ROM, shows 1.0 working), and leave out do_offset
#    attribute latching (Cycle1ShadeTriangle16BPP: exact on the triangles
#    without it). SetPrimColor32BPP and Cycle1ShadeTriangle32BPP have the
#    16-bit pictures as references.
#  - 8BPP/.../InternalPalette: the reference holds colours no 16-bit
#    scan-out can produce; it is not a framebuffer view.
#  - RDPModeInput: renders only after a button press; the reference shows
#    a state reached by input.
#  - AlphaCompare: the dithered alpha compare uses the hardware's random
#    noise; parallel-rdp's hash stands in for it.
#  - CombinerLongTailConstants: the first pixel drawn is not dithered in
#    the reference; every other pixel matches.
#  - LoadTLUT_*_b32: LOAD_TLUT into a 32-bit tile writes TMEM on hardware;
#    parallel-rdp (and this port) treat it as unsupported.
"""


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    parts = data.split(maxsplit=4)
    if parts[0] != b"P6" or int(parts[3]) != 255:
        raise ValueError("not an 8-bit P6 PPM")
    w, h = int(parts[1]), int(parts[2])
    pixels = parts[4]
    if len(pixels) < w * h * 3:
        raise ValueError("short PPM")
    return w, h, pixels[: w * h * 3]


def vi_fields(stdout):
    fields = {}
    for line in stdout.splitlines():
        if line.startswith("vi: "):
            for item in line[4:].split():
                key, _, value = item.partition("=")
                fields[key] = value
    return fields


def count_diff(ours, refb, w, h, mask, row_of):
    """Pixels differing at `mask` precision; row_of maps a reference row to
    a dumped framebuffer row."""
    diff = 0
    bad = bytearray(w * h)
    for y in range(h):
        fy = row_of(y)
        o_row, r_row = 3 * w * fy, 3 * w * y
        if ours[o_row : o_row + 3 * w] == refb[r_row : r_row + 3 * w]:
            continue
        for x in range(w):
            o, r = o_row + 3 * x, r_row + 3 * x
            if (ours[o] & mask, ours[o + 1] & mask, ours[o + 2] & mask) != (
                refb[r] & mask,
                refb[r + 1] & mask,
                refb[r + 2] & mask,
            ):
                diff += 1
                bad[y * w + x] = 1
    return diff, bad


def run_one(args, rom, tmp):
    rel = os.path.relpath(rom, args.root).replace(os.sep, "/")  # baseline keys use forward slashes
    png = rom[:-4] + ".png"
    if not os.path.exists(png):
        return rel, None, "no reference PNG"
    ref = Image.open(png).convert("RGB")
    w, h = ref.size
    out = os.path.join(tmp, rel.replace("/", "_") + ".ppm")
    cmd = [args.runner, rom, "--frames", str(args.frames), "--dump-fb", out, "--fb-height", str(h)]
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=args.timeout)
    except subprocess.TimeoutExpired:
        return rel, None, "timeout"
    if proc.returncode != 0 or not os.path.exists(out):
        return rel, None, "runner failed (%d): %s" % (proc.returncode, proc.stderr.strip()[-200:])
    vi = vi_fields(proc.stdout)
    try:
        ctrl = int(vi["ctrl"], 16)
        v_video = int(vi["v_video"], 16)
        y_scale = int(vi["y_scale"], 16)
        ow, oh, ours = read_ppm(out)
    except (KeyError, OSError, ValueError) as e:
        return rel, None, "bad dump: %s" % e
    if ow * 2 == w and oh == h:
        # An 8-bit colour image shown by the VI as half as many 16-bit
        # pixels (8BPP/.../InternalPalette): double them horizontally.
        ours = b"".join(ours[i : i + 3] * 2 for i in range(0, len(ours), 3))
        ow = w
    if ow != w or oh != h:
        return rel, None, "size %dx%d, reference %dx%d" % (ow, oh, w, h)
    mask = 0xF8 if (ctrl & 3) != 3 else 0xFF
    refb = ref.tobytes()
    # The lines the VI shows: V_VIDEO's half-lines scaled by Y_SCALE.
    shown = ((((v_video & 0x3FF) - ((v_video >> 16) & 0x3FF)) & 0x3FF) >> 1) * (y_scale & 0xFFF) >> 10
    best = count_diff(ours, refb, w, h, mask, lambda y: y) + ("raw",)
    if 0 < shown < h and best[0]:
        stretched = count_diff(ours, refb, w, h, mask, lambda y: (2 * y + 1) * shown // (2 * h))
        if stretched[0] < best[0]:
            best = stretched + ("stretched",)
    diff, bad, how = best
    if args.diff_dir and diff:
        os.makedirs(args.diff_dir, exist_ok=True)
        img = Image.frombytes("RGB", (w, h), bytes(ours))
        img.save(os.path.join(args.diff_dir, rel.replace("/", "_") + ".ours.png"))
        mimg = Image.frombytes("L", (w, h), bytes(b * 255 for b in bad))
        mimg.save(os.path.join(args.diff_dir, rel.replace("/", "_") + ".mask.png"))
    return rel, diff, None if diff is not None else "no result", how


def load_baseline(path):
    base = {}
    if not os.path.exists(path):
        return base
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            name, count = line.rsplit(None, 1)
            base[name] = int(count)
    return base


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    repo = os.path.dirname(os.path.dirname(here))
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--runner", required=True)
    p.add_argument("--data-dir", default=os.path.join(repo, "roms", "n64"))
    p.add_argument("--baseline", default=os.path.join(repo, "tests", "n64", "rdp_baseline.txt"))
    p.add_argument("--update-baseline", action="store_true")
    p.add_argument("--filter", default="")
    p.add_argument("--frames", type=int, default=30)
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 1)
    p.add_argument("--timeout", type=int, default=120)
    p.add_argument("--diff-dir", default="")
    args = p.parse_args()

    args.root = os.path.join(args.data_dir, "krom", "RDP")
    roms = []
    for dirpath, _, files in os.walk(args.root):
        roms += [os.path.join(dirpath, f) for f in files if f.upper().endswith(".N64")]
    roms.sort()
    roms = [r for r in roms if args.filter in os.path.relpath(r, args.root)]
    if not roms:
        print("no ROMs under %s matching '%s'" % (args.root, args.filter))
        return 1

    with tempfile.TemporaryDirectory() as tmp:
        with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
            results = list(pool.map(lambda r: run_one(args, r, tmp), roms))
    results = [r if len(r) == 4 else r + ("",) for r in results]

    base = load_baseline(args.baseline)
    failures = 0
    groups = {}
    for rel, diff, err, how in results:
        group = rel.split("/")[0]
        g = groups.setdefault(group, [0, 0])
        g[1] += 1
        if diff == 0:
            g[0] += 1
        if err:
            print("ERROR %s: %s" % (rel, err))
            failures += 1
            continue
        status = "PASS" if diff == 0 else "DIFF %d" % diff
        if not args.update_baseline:
            if rel not in base:
                status += "  (not in baseline)"
                failures += 1
            elif base[rel] != diff:
                status += "  (baseline %d)" % base[rel]
                failures += 1
        print("%s %s [%s]" % (status, rel, how))

    if not args.filter and not args.update_baseline:
        seen = {rel for rel, _, _, _ in results}
        for name in sorted(set(base) - seen):
            print("MISSING %s (in the baseline, not in the data)" % name)
            failures += 1

    print("\nper group (exact / total):")
    for group in sorted(groups):
        print("  %-28s %d / %d" % (group, groups[group][0], groups[group][1]))
    exact = sum(g[0] for g in groups.values())
    print("exact: %d / %d" % (exact, len(results)))

    if args.update_baseline:
        if any(err for _, _, err, _ in results):
            print("not writing the baseline: some ROMs failed to run")
            return 1
        if args.filter:
            base.update({rel: diff for rel, diff, _, _ in results})
            out = base
        else:
            out = {rel: diff for rel, diff, _, _ in results}
        with open(args.baseline, "w") as f:
            f.write(BASELINE_HEADER)
            for name in sorted(out):
                f.write("%s %d\n" % (name, out[name]))
        print("wrote %s" % args.baseline)
        return 0
    if failures:
        print("FAIL: %d ROM(s) differ from the baseline or did not run" % failures)
        return 1
    print("PASS: every ROM matches the baseline")
    return 0


if __name__ == "__main__":
    sys.exit(main())
