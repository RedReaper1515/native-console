#!/usr/bin/env python3
"""Every SDL frontend quits through a quit_sdl() that flushes stdout and
stderr before it calls SDL_Quit().

On Windows a program that calls SDL_Quit() within a moment of
SDL_Init(SDL_INIT_GAMECONTROLLER) now and then exits with whatever is still
in stdout's buffer never written: the exit code is right and the text is
gone. A menu self-test's verify phase prints its verdict and quits in exactly
that shape, and its script reads the verdict from stdout.

It happens one or two times in a hundred runs, on Windows, so a test that
runs a frontend cannot pin it. This one reads the sources instead: in a
frontend, SDL_Quit() is called in one place, quit_sdl(), after both streams
are flushed.

    python tests/test_frontends_flush_before_quit.py [--root REPO]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# The frontends: one per console runtime, and the Game Boy's.
FRONTEND_GLOBS = ("runtime-*/platform/*_sdl.cpp", "runtime/src/platform_sdl.cpp")
# The fewest files the patterns must find: six consoles had a frontend when
# this was written, and a pattern that stops matching must not pass.
MIN_FRONTENDS = 6

QUIT_CALL = re.compile(r"\bSDL_Quit\s*\(")
QUIT_SDL = re.compile(r"\bstatic\s+void\s+quit_sdl\s*\(\s*void\s*\)\s*\{(?P<body>[^{}]*)\}")
FLUSHES = {
    "fflush(stdout)": re.compile(r"\bfflush\s*\(\s*stdout\s*\)"),
    "fflush(stderr)": re.compile(r"\bfflush\s*\(\s*stderr\s*\)"),
}


def blank_comments_and_strings(text: str) -> str:
    """The source with comments and literals turned to spaces, newlines kept,
    so that what is left is code at the offsets it had."""
    out = []
    i, n = 0, len(text)
    while i < n:
        two = text[i:i + 2]
        if two == "//":
            end = text.find("\n", i)
            end = n if end < 0 else end
        elif two == "/*":
            end = text.find("*/", i + 2)
            end = n if end < 0 else end + 2
        elif text[i] in "\"'":
            end = i + 1
            while end < n and text[end] != text[i] and text[end] != "\n":
                end += 2 if text[end] == "\\" else 1
            end = min(end + 1, n)
        else:
            out.append(text[i])
            i += 1
            continue
        out.append("".join("\n" if c == "\n" else " " for c in text[i:end]))
        i = end
    return "".join(out)


def check(path: Path, name: str) -> list[str]:
    code = blank_comments_and_strings(path.read_text(encoding="utf-8"))
    calls = [m.start() for m in QUIT_CALL.finditer(code)]
    if not calls:
        return []  # a frontend that never calls SDL_Quit() has nothing to lose this way
    helper = QUIT_SDL.search(code)
    problems = []
    if not helper:
        problems.append(f"{name}: calls SDL_Quit() but has no static void quit_sdl(void) to call it from")
    else:
        body = helper.group("body")
        quit_at = QUIT_CALL.search(body)
        for call, pattern in FLUSHES.items():
            flush_at = pattern.search(body)
            if not quit_at or not flush_at or flush_at.start() > quit_at.start():
                problems.append(f"{name}: quit_sdl() must call {call} before SDL_Quit()")
    for at in calls:
        if helper and helper.start("body") <= at < helper.end("body"):
            continue
        line = code.count("\n", 0, at) + 1
        problems.append(f"{name}:{line}: SDL_Quit() called directly; call quit_sdl(), which flushes stdout "
                        "and stderr first")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1], help="repository root")
    args = parser.parse_args()

    frontends = sorted({path for pattern in FRONTEND_GLOBS for path in args.root.glob(pattern)})
    if len(frontends) < MIN_FRONTENDS:
        print(f"found {len(frontends)} frontend sources under {args.root}, expected at least {MIN_FRONTENDS}")
        return 1
    problems = []
    for path in frontends:
        problems += check(path, path.relative_to(args.root).as_posix())
    for problem in problems:
        print(problem)
    print(f"{len(frontends)} frontends: {'FAIL' if problems else 'PASS'}")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
