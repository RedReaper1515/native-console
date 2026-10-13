#!/usr/bin/env python3
"""Convert Jaklyy/polyrastertest into a C header for tests/nds/test_nds_gx_raster.c.

polyrastertest (MIT) is a DS program that draws small scenes with the 3D
engine and compares what the display capture unit saw with a recording made
on real hardware (a New 3DS XL), `data/rastertest.data`. This script writes
tests/nds/gx/polyrastertest_data.h: the scenes as plain C tables and the
recording as a byte array, so the test needs no file at run time.

Which commit. The recording was last made for release v1.0.2 and was not
made again afterwards: the repository's later commits add and change scenes
(and move them to source/tests.h) while data/rastertest.data stays the
v1.0.2 file. So the scenes that have hardware data are the ones in
source/main.h of the release tag v1.0.2-b, commit
550c208e898b546c92defa57c0a0143f00da2f92, and that is what is converted. The
prebuilt ROM tools/fetch_nds_tests.py fetches is the same release.

    python tools/nds/gx_polyraster_convert.py --fetch     download the three pinned files, then convert
    python tools/nds/gx_polyraster_convert.py             convert from --source DIR
    python tools/nds/gx_polyraster_convert.py --check     fail if the header is stale
    python tools/nds/gx_polyraster_convert.py --list      list the scenes
    python tools/nds/gx_polyraster_convert.py --show 12   print what hardware drew for scene 12

--source DIR holds main.h, rastertest.data and LICENSE of that commit, either
flat or in the repository's layout (source/, data/); the default is
build-agents/nds-gx/ref/prt/<commit>. Scenes are numbered from 1, as the
program's own screen numbers them.
"""
import argparse
import hashlib
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
OUTPUT = ROOT / "tests/nds/gx/polyrastertest_data.h"

REPOSITORY = "Jaklyy/polyrastertest"
COMMIT = "550c208e898b546c92defa57c0a0143f00da2f92"
RELEASE = "v1.0.2-b"
# Path in the repository, and the SHA-256 of the file at COMMIT.
PINNED = {
    "source/main.h": "3361238d391f92bc659b34a7a0b9913eb787ea3f18c44253029a145618fe6347",
    "data/rastertest.data": "b2cdf4f86967cc0c686f894bd7583e94698a3f0b1c695686b29f93fc03684752",
    "LICENSE": "192560000627c4fc37e095a31e6ae6b288e534451e51c572c8a79e3bb146eefe",
}
DEFAULT_SOURCE = ROOT / "build-agents/nds-gx/ref/prt" / COMMIT

NUM_VERTS = 4
NUM_AXIS = 3

# libnds names the scene definitions use (videoGL.h).
LIBNDS = {
    "POLY_CULL_NONE": 3 << 6,
    "POLY_CULL_FRONT": 1 << 6,
    "POLY_CULL_BACK": 2 << 6,
    "GL_TEXTURE_2D": 1 << 0,
    "GL_TOON_HIGHLIGHT": 1 << 1,
    "GL_ALPHA_TEST": 1 << 2,
    "GL_BLEND": 1 << 3,
    "GL_ANTIALIAS": 1 << 4,
    "GL_OUTLINE": 1 << 5,
    "GL_FOG_ONLY_ALPHA": 1 << 6,
    "GL_FOG": 1 << 7,
    "GL_MAX_DEPTH": 0x7FFF,
}

# main.h: the constants above the tables, and the default member initialisers
# of TestData and Polygon.
MAIN_H = {
    "Opaque": 31 << 16,
    "Wireframe": 0 << 16,
    "Trans": lambda opacity: opacity << 16,
    "PolyID": lambda ident: ident << 24,
    "ColorStock": 0b111111111111111,
    "ColorMissing": 0b000000000011111,
    "ColorOverdraw": 0b111110000011111,
    "ColorMatch": 0b000001111100000,
    "ColorWrongTex": 0b111110000000000,
    "ColorVoid": 0b000000000000000,
    "true": 1,
    "false": 0,
}
DEFAULT_ATTR = (31 << 16) | (3 << 6)
DEFAULT_COLORS = [0x7FFF] * 4
DEFAULT_PROJECTION = [131586, 0, 0, 0,
                      0, -175677, 0, 0,
                      0, 0, -1024, 0,
                      16, 21, -4096, 4096]
DEFAULT_VIEWPORT = (0 << 0) + (0 << 8) + (255 << 16) + (191 << 24)
DEFAULT_OUTLINE = [0x7FFF, 0, 0, 0, 0, 0, 0, 0]
DATA_VERSION = 2

TEST_FIELDS = {"Vertices", "PolyAttr", "VertexColors", "Disp3DCnt", "Viewport", "Projection",
               "OutlineColors", "RearID", "RearDepth", "ColorMode", "ExtendedTestData"}
POLYGON_FIELDS = {"Vertices", "PolyAttr", "VertexColors"}


def source_file(source, name):
    """A pinned file in --source, in the repository's layout or flat."""
    for candidate in (source / name, source / Path(name).name):
        if candidate.is_file():
            return candidate
    return None


def fetch(source):
    source.mkdir(parents=True, exist_ok=True)
    for name, digest in PINNED.items():
        target = source / Path(name).name
        if target.is_file() and hashlib.sha256(target.read_bytes()).hexdigest() == digest:
            continue
        url = f"https://raw.githubusercontent.com/{REPOSITORY}/{COMMIT}/{name}"
        with urllib.request.urlopen(url, timeout=60) as response:  # noqa: S310 (fixed https URL)
            data = response.read()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"{url}: not the pinned file")
        target.write_bytes(data)
        print(f"fetched {target}")


def read_pinned(source):
    files = {}
    for name, digest in PINNED.items():
        path = source_file(source, name)
        if path is None:
            raise FileNotFoundError(f"{source}: no {Path(name).name}; run with --fetch")
        data = path.read_bytes()
        if hashlib.sha256(data).hexdigest() != digest:
            raise ValueError(f"{path}: not the file of {REPOSITORY} {RELEASE} (commit {COMMIT})")
        files[name] = data
    return files


def strip_comments(text):
    """Blank out comments, keeping every character's offset. Returns the
    stripped text and the line comments as (offset, text)."""
    out = list(text)
    comments = []
    i = 0
    while i < len(text):
        if text.startswith("//", i):
            end = text.find("\n", i)
            if end < 0:
                end = len(text)
            comments.append((i, text[i + 2:end].strip()))
            for k in range(i, end):
                out[k] = " "
            i = end
        elif text.startswith("/*", i):
            end = text.find("*/", i + 2)
            end = len(text) if end < 0 else end + 2
            for k in range(i, end):
                if out[k] != "\n":
                    out[k] = " "
            i = end
        else:
            i += 1
    return "".join(out), comments


class Parser:
    """Just enough C++ to read the two tables: nested brace lists whose items
    are either `.Name = value`, `.Name {list}` or an expression."""

    def __init__(self, text, env):
        self.text = text
        self.pos = 0
        self.env = env

    def skip(self):
        while self.pos < len(self.text) and self.text[self.pos].isspace():
            self.pos += 1

    def peek(self):
        self.skip()
        return self.text[self.pos] if self.pos < len(self.text) else ""

    def expect(self, char):
        if self.peek() != char:
            line = self.text.count("\n", 0, self.pos) + 1
            raise SyntaxError(f"line {line}: expected {char!r}, found {self.peek()!r}")
        self.pos += 1

    def expression(self):
        self.skip()
        start = self.pos
        depth = 0
        while self.pos < len(self.text):
            c = self.text[self.pos]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
            elif depth == 0 and c in ",}":
                break
            self.pos += 1
        source = self.text[start:self.pos].strip()
        # The text is the pinned (hash-checked) test table; its expressions
        # are sums and ors of the names above.
        return int(eval(source, {"__builtins__": {}}, self.env))  # noqa: S307

    def value(self):
        """A brace list: a dict when its items are designated, else a list."""
        if self.peek() != "{":
            return self.expression()
        self.expect("{")
        start = self.pos
        fields, items = {}, []
        while self.peek() != "}":
            if self.peek() == ".":
                self.pos += 1
                name = re.match(r"\w+", self.text[self.pos:]).group(0)
                self.pos += len(name)
                if self.peek() == "=":
                    self.pos += 1
                fields[name] = self.value()
            else:
                items.append(self.value())
            if self.peek() == ",":
                self.pos += 1
        self.expect("}")
        if fields:
            fields["_offset"] = start
            return fields
        return items


def pad(values, count, fill=0):
    values = list(values)
    if len(values) > count:
        raise ValueError(f"{len(values)} initialisers for {count} elements")
    return values + [fill] * (count - len(values))


def vertices(value):
    result = [pad(v, NUM_AXIS) for v in pad(value, NUM_VERTS, [])]
    for vertex in result:
        for coordinate in vertex:
            if not -0x8000 <= coordinate <= 0x7FFF:
                raise ValueError(f"{coordinate} does not fit an s16")
    return result


def polygon(raw):
    unknown = set(raw) - POLYGON_FIELDS - {"_offset"}
    if unknown:
        raise ValueError(f"unknown Polygon fields {sorted(unknown)}")
    return {
        "vertices": vertices(raw["Vertices"]),
        "attr": raw.get("PolyAttr", DEFAULT_ATTR),
        # An array member with fewer initialisers than elements is
        # zero-filled, not filled from the default member initialiser.
        "colors": pad(raw["VertexColors"], 4) if "VertexColors" in raw else DEFAULT_COLORS,
    }


def load_tests(main_h):
    if int(re.search(r"DataVersion\s*=\s*(\d+)", main_h).group(1)) != DATA_VERSION:
        raise ValueError("main.h: unexpected DataVersion")
    stripped, comments = strip_comments(main_h)
    env = dict(LIBNDS)
    env.update(MAIN_H)

    def table(name):
        parser = Parser(stripped, env)
        parser.pos = stripped.index("=", stripped.index(name)) + 1
        return parser.value()

    def describe(offset):
        """The category and sub-category headings in force at an entry, and
        the comment line directly above it."""
        category, sub = "", ""
        for where, text in comments:
            if where > offset:
                break
            heading = re.match(r"(Category|Catgory|Sub Cat):\s*(.*?)[\s=-]*$", text)
            if heading and heading.group(1) == "Sub Cat":
                sub = heading.group(2)
            elif heading:
                category, sub = heading.group(2), ""
        # The comment lines directly above; the last one names the scene,
        # unless it is a note to self.
        line_start = main_h.rfind("\n", 0, offset) + 1
        lines = []
        while line_start > 0:
            previous_start = main_h.rfind("\n", 0, line_start - 1) + 1
            previous = main_h[previous_start:line_start].strip()
            if not previous.startswith("//"):
                break
            lines.insert(0, previous[2:].strip())
            line_start = previous_start
        named = [text for text in lines if text and not text.lower().startswith("todo")]
        name = named[-1] if named else ""
        return category, sub, name

    tests = []
    for raw in table("constexpr TestData Tests[]"):
        offset = raw.pop("_offset")
        unknown = set(raw) - TEST_FIELDS
        if unknown:
            raise ValueError(f"unknown TestData fields {sorted(unknown)}")
        category, sub, name = describe(offset)
        test = polygon({k: v for k, v in raw.items() if k in POLYGON_FIELDS})
        test.update({
            "line": main_h.count("\n", 0, offset) + 1,
            "category": category,
            "sub": sub,
            "name": name,
            "disp3dcnt": raw.get("Disp3DCnt", 0),
            "viewport": raw.get("Viewport", DEFAULT_VIEWPORT),
            "projection": pad(raw["Projection"], 16) if "Projection" in raw else DEFAULT_PROJECTION,
            "outline": pad(raw["OutlineColors"], 8) if "OutlineColors" in raw else DEFAULT_OUTLINE,
            "rear_id": raw.get("RearID", 63),
            "rear_depth": raw.get("RearDepth", LIBNDS["GL_MAX_DEPTH"]),
            "color_mode": raw.get("ColorMode", 0),
            "ext": raw.get("ExtendedTestData", 0),
        })
        tests.append(test)

    extended = []
    for raw in table("constexpr ExtTestData ExtendedTests[]"):
        polygons = [polygon(p) for p in raw["Polygons"]]
        if raw.get("NumPolygons", 1) != len(polygons):
            raise ValueError("ExtendedTests: NumPolygons does not match the list")
        extended.append(polygons)

    for test in tests:
        if test["ext"] > len(extended):
            raise ValueError(f"line {test['line']}: ExtendedTestData {test['ext']} does not exist")
    return tests, extended


class Bits:
    """The recording: a big-endian bit stream."""

    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, count):
        value = 0
        for _ in range(count):
            if (self.pos >> 3) >= len(self.data):
                raise ValueError("rastertest.data: ran off the end")
            value = (value << 1) | ((self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1)
            self.pos += 1
        return value


def multispan(test):
    """main.cpp: a scene's lines list every span, not just the first, when
    its first polygon is wireframe or when it has extended polygons."""
    return (test["attr"] & (31 << 16)) == 0 or test["ext"] > 0


def decode(tests, data):
    """Per scene, 192 lines of [(start, end, [colour...])]. The colour list
    is empty where the recording has none: scenes with colour mode 0, and
    (a slip in this release's record()) spans that reach column 255."""
    bits = Bits(data)
    if bits.take(16) != DATA_VERSION:
        raise ValueError("rastertest.data: unexpected version")
    frames = []
    for test in tests:
        colour_bits = min(test["color_mode"], 3) * 5
        frame = []
        for _ in range(192):
            spans = []
            while bits.take(1):
                start, end = bits.take(8), bits.take(8)
                if end < start:
                    raise ValueError("rastertest.data: a span ends before it starts")
                colours = []
                if colour_bits and end != 255:
                    colours = [bits.take(colour_bits) for _ in range(start, end + 1)]
                spans.append((start, end, colours))
                if not multispan(test):
                    break
            frame.append(spans)
        frames.append(frame)
    # record() flushes the last partial byte, and one more when the stream
    # ended on a byte boundary.
    used = (bits.pos + 7) // 8
    if not 0 <= len(data) - used <= 1:
        raise ValueError(f"rastertest.data: decoded {used} of {len(data)} bytes")
    return frames


def show(tests, extended, frames, number):
    test, frame = tests[number - 1], frames[number - 1]
    print(f"scene {number} (main.h line {test['line']}): {test['category']} / {test['sub']} / {test['name']}")
    print(f"  vertices {test['vertices']} attr {test['attr']:#010x} colors {[hex(c) for c in test['colors']]}")
    print(f"  disp3dcnt {test['disp3dcnt']:#06x} outline {[hex(c) for c in test['outline']]} rear id {test['rear_id']}"
          f" colour mode {test['color_mode']}")
    if test["ext"]:
        for poly in extended[test["ext"] - 1]:
            print(f"  + vertices {poly['vertices']} attr {poly['attr']:#010x} colors {[hex(c) for c in poly['colors']]}")
    for y, spans in enumerate(frame):
        if not spans:
            continue
        parts = []
        for start, end, colours in spans:
            text = f"{start}-{end}"
            if colours:
                runs, last = [], None
                for x, colour in zip(range(start, end + 1), colours):
                    if colour != last:
                        runs.append(f"{x}:{colour:04X}")
                        last = colour
                text += "[" + " ".join(runs) + "]"
            parts.append(text)
        print(f"  y={y:3d}: " + "  ".join(parts))


def c_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(tests, extended, data, license_text):
    out = []
    out.append("/*")
    out.append(" * polyrastertest's scenes and its hardware recording, as C tables.")
    out.append(" *")
    out.append(" * Generated by tools/nds/gx_polyraster_convert.py from Jaklyy/polyrastertest")
    out.append(f" * (https://github.com/{REPOSITORY}), release {RELEASE}, commit")
    out.append(f" * {COMMIT}: the scenes in source/main.h and")
    out.append(" * data/rastertest.data, which its author recorded on a New 3DS XL. Do not edit;")
    out.append(" * run the script again.")
    out.append(" *")
    out.append(" * polyrastertest is distributed under this license:")
    out.append(" *")
    for line in license_text.strip().splitlines():
        out.append((" * " + line).rstrip())
    out.append(" */")
    out.append("#ifndef NDS_GX_POLYRASTERTEST_DATA_H")
    out.append("#define NDS_GX_POLYRASTERTEST_DATA_H")
    out.append("")
    out.append("#include <stdint.h>")
    out.append("")
    out.append(f"#define PRT_NUM_TESTS {len(tests)}")
    out.append(f"#define PRT_NUM_EXTENDED {len(extended)}")
    out.append(f"#define PRT_DATA_VERSION {DATA_VERSION}")
    out.append("")
    out.append("typedef struct PrtPolygon {")
    out.append("    int16_t vertices[4][3]; /* x, y, z as VTX_16 takes them; x of the 4th = -512: a triangle */")
    out.append("    uint32_t attr;          /* POLYGON_ATTR */")
    out.append("    uint16_t colors[4];     /* COLOR before each vertex */")
    out.append("} PrtPolygon;")
    out.append("")
    out.append("typedef struct PrtTest {")
    out.append("    PrtPolygon first;")
    out.append("    uint16_t disp3dcnt;")
    out.append("    uint32_t viewport;")
    out.append("    int32_t projection[16];")
    out.append("    uint16_t outline[8];    /* EDGE_COLOR */")
    out.append("    uint8_t rear_id;        /* CLEAR_COLOR's polygon ID; the rear plane is opaque black */")
    out.append("    uint16_t rear_depth;    /* CLEAR_DEPTH */")
    out.append("    uint8_t color_mode;     /* what the recording keeps per pixel: 0 nothing, 1 red, 2 red and green, 3 all */")
    out.append("    uint8_t extended;       /* 0, or 1 + the index in prt_extended of a polygon drawn after the first */")
    out.append("    uint16_t line;          /* where the scene is in main.h */")
    out.append("    const char *category;")
    out.append("    const char *sub;")
    out.append("    const char *name;       /* the author's comment on the scene */")
    out.append("} PrtTest;")
    out.append("")

    def poly_text(poly):
        verts = ", ".join("{" + ", ".join(str(c) for c in v) + "}" for v in poly["vertices"])
        colors = ", ".join(f"0x{c:04X}" for c in poly["colors"])
        return f"{{{{{verts}}}, 0x{poly['attr']:08X}u, {{{colors}}}}}"

    out.append("static const PrtTest prt_tests[PRT_NUM_TESTS] = {")
    for index, test in enumerate(tests):
        out.append(f"    /* {index + 1} */")
        out.append(f"    {{{poly_text(test)},")
        out.append(f"     0x{test['disp3dcnt']:04X}, 0x{test['viewport']:08X}u,")
        out.append("     {" + ", ".join(str(v) for v in test["projection"]) + "},")
        out.append("     {" + ", ".join(f"0x{c:04X}" for c in test["outline"]) + "},")
        out.append(f"     {test['rear_id']}, 0x{test['rear_depth']:04X}, {test['color_mode']}, {test['ext']}, {test['line']},")
        out.append(f"     {c_string(test['category'])}, {c_string(test['sub'])},")
        out.append(f"     {c_string(test['name'])}}},")
    out.append("};")
    out.append("")
    out.append("static const PrtPolygon prt_extended[PRT_NUM_EXTENDED] = {")
    for index, polygons in enumerate(extended):
        if len(polygons) != 1:
            raise ValueError("this release has one polygon per extended scene")
        out.append(f"    /* {index + 1} */ {poly_text(polygons[0])},")
    out.append("};")
    out.append("")
    out.append("/* data/rastertest.data: a big-endian bit stream. 16 bits of version; then per")
    out.append(" * scene, per scanline: a 1 bit and an 8-bit first and last column for a run of")
    out.append(" * pixels that are not black, or a 0 bit for none. When the scene records")
    out.append(" * colour the run is followed by 5, 10 or 15 bits per pixel (the low bits of")
    out.append(" * the captured BGR555 pixel), except for a run that reaches column 255, whose")
    out.append(" * colours this release's recorder left out. Scenes whose first polygon is")
    out.append(" * wireframe, or that have an extended polygon, list every run on the line and")
    out.append(" * end the list with a 0 bit; the others record only the first run. */")
    out.append(f"#define PRT_DATA_SIZE {len(data)}")
    out.append("static const uint8_t prt_data[PRT_DATA_SIZE] = {")
    for i in range(0, len(data), 20):
        out.append("    " + ",".join(str(b) for b in data[i:i + 20]) + ",")
    out.append("};")
    out.append("")
    out.append("#endif /* NDS_GX_POLYRASTERTEST_DATA_H */")
    return "\n".join(out) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE,
                        help="directory with main.h, rastertest.data and LICENSE of the pinned commit")
    parser.add_argument("--fetch", action="store_true", help="download the pinned files into --source first")
    parser.add_argument("--check", action="store_true", help="fail if the header on disk is not what would be written")
    parser.add_argument("--show", type=int, metavar="N", help="print what hardware drew for scene N (from 1)")
    parser.add_argument("--list", action="store_true", help="list the scenes")
    args = parser.parse_args()

    try:
        if args.fetch:
            fetch(args.source)
        files = read_pinned(args.source)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1
    tests, extended = load_tests(files["source/main.h"].decode("utf-8"))
    data = files["data/rastertest.data"]
    frames = decode(tests, data)

    if args.list:
        for index, test in enumerate(tests):
            flags = ("m" if multispan(test) else "-") + str(test["color_mode"])
            print(f"{index + 1:3d} line {test['line']:3d} {flags} {test['category']} / {test['sub']} / {test['name']}")
        return 0
    if args.show:
        show(tests, extended, frames, args.show)
        return 0

    text = render(tests, extended, data, files["LICENSE"].decode("utf-8"))
    if args.check:
        if not OUTPUT.is_file() or OUTPUT.read_text(encoding="utf-8") != text:
            print(f"{OUTPUT}: stale; run tools/nds/gx_polyraster_convert.py", file=sys.stderr)
            return 1
        print(f"{OUTPUT}: up to date ({len(tests)} scenes)")
        return 0
    OUTPUT.parent.mkdir(parents=True, exist_ok=True)
    OUTPUT.write_text(text, encoding="utf-8", newline="\n")
    print(f"{OUTPUT}: {len(tests)} scenes, {len(extended)} extended polygons, {len(data)} bytes of recording")
    return 0


if __name__ == "__main__":
    sys.exit(main())
