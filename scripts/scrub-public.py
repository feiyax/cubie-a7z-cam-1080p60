#!/usr/bin/env python3
"""Catch accidental private artifacts in tracked and nonignored candidate files."""

import pathlib
import re
import subprocess
import sys

root = pathlib.Path(__file__).resolve().parents[1]
files = (
    subprocess.check_output(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root,
    )
    .decode()
    .split("\0")
)
# Build sensitive patterns from pieces so this checker does not flag its own source.
patterns = [
    re.compile("/home/" + "user"),
    re.compile("/mnt/" + "nvme"),
    re.compile(r"192\.168\.\d+\.\d+"),
    re.compile(r"10\.\d+\.\d+\.\d+"),
]
forbidden = {
    ".so",
    ".ko",
    ".o",
    ".pyc",
    ".h264",
    ".mp4",
    ".jpg",
    ".jpeg",
    ".png",
    ".bin",
    ".log",
}
failed = []
for name in sorted(set(files) - {""}):
    path = root / name
    if not path.is_file():
        continue
    if path.suffix.lower() in forbidden:
        failed.append(name + ": generated/private artifact")
        continue
    text = path.read_text(errors="replace")
    if any(p.search(text) for p in patterns):
        failed.append(name + ": private path/network pattern")
if failed:
    print("\n".join(failed), file=sys.stderr)
    sys.exit(1)
print(
    (
        "Public artifact/path scan passed; manual licensing/privacy "
        "review is still required."
    )
)
