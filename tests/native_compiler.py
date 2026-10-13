"""Locate a C compiler for test harnesses and build commands for it."""

from __future__ import annotations

import os
import shutil
from pathlib import Path


def find_c_compiler() -> str | None:
    """Honor $CC, then look for the usual POSIX drivers, then MSVC's cl."""
    configured = os.environ.get("CC")
    if configured:
        return configured
    for name in ("cc", "gcc", "clang", "cl"):
        found = shutil.which(name)
        if found:
            return found
    return None


def is_msvc(compiler: str) -> bool:
    return Path(compiler).stem.lower() == "cl"


def executable_path(path: Path) -> Path:
    """Give the output the extension the host needs to launch it."""
    return path.with_suffix(".exe") if os.name == "nt" else path


def compile_command(
    compiler: str,
    *,
    sources: list[Path],
    include_dirs: list[Path],
    output: Path,
) -> list[str]:
    """Compile C11 sources with warnings as errors into one executable.

    MSVC writes object files into the working directory, so run the command
    with a scratch ``cwd``.
    """
    if is_msvc(compiler):
        command = [compiler, "/nologo", "/std:c11", "/W4", "/WX"]
        for directory in include_dirs:
            command.append(f"/I{directory}")
        command.extend(str(source) for source in sources)
        command.append(f"/Fe:{output}")
        return command
    command = [compiler, "-std=c11", "-Wall", "-Wextra", "-Werror"]
    for directory in include_dirs:
        command.extend(["-I", str(directory)])
    command.extend(str(source) for source in sources)
    command.extend(["-o", str(output)])
    return command
