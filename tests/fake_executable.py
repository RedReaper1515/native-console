"""Write Python-scripted stand-ins for game executables that tests launch."""

from __future__ import annotations

import os
import sys
from pathlib import Path


def write_fake_executable(path: Path, source: str) -> Path:
    """Store ``source`` (a Python program) so launching the returned path runs it.

    POSIX executes the script through its shebang. Windows cannot execute a
    script file directly, so the script is written beside a ``.cmd`` wrapper
    that hands it to the current interpreter. Callers must use the returned
    path everywhere the executable is referenced, including when hashing it.
    """
    if os.name == "nt":
        script = path.with_name(path.name + ".py")
        script.write_text(source, encoding="utf-8", newline="\n")
        wrapper = path.with_name(path.name + ".cmd")
        wrapper.write_text(
            f'@"{sys.executable}" "{script}" %*\r\n', encoding="utf-8", newline=""
        )
        return wrapper
    path.write_text(source, encoding="utf-8", newline="\n")
    path.chmod(path.stat().st_mode | 0o111)
    return path
