#!/usr/bin/env python3
"""Report upstream changes to the PGTK functions the Wayland backend copies.

The backend's drawing code started as copies of functions in
src/pgtkterm.c, renamed from pgtk_ to wayland_.  Upstream fixes to the
originals do not reach the copies by themselves.  pgtk-sync.json records
a hash of each original as of the last time its copy was brought up to
date.  After a rebase, run

  pgtk-drift.py            list originals that changed since then
  pgtk-drift.py --diff F   compare original F with its copy
  pgtk-drift.py --record   accept the current state, once ported

Comments and whitespace do not count as changes.  Run by `make check-drift'."""

# This file is part of GNU Emacs, as modified by the native Wayland
# backend fork.  It is free software under the GNU General Public
# License, version 3 or later.

import argparse
import difflib
import hashlib
import json
from pathlib import Path
import re
import sys

HERE = Path(__file__).resolve().parent
RECORD = HERE / "pgtk-sync.json"
# A copy is tracked while at least this share of its lines match the
# original; below that the two have diverged by design.
TRACKED = 0.5


def functions(path):
    """Return {name: lines} for the functions defined in the C file PATH.
    GNU style puts the name in column 0 and the closing brace alone."""
    lines = path.read_text(encoding="utf-8", errors="replace").split("\n")
    found, i = {}, 0
    while i < len(lines):
        match = re.match(r"([A-Za-z_]\w*) \(", lines[i])
        if match and i and not lines[i - 1].rstrip().endswith(";"):
            end = i
            while end < len(lines) and not lines[end].startswith("}"):
                end += 1
            if end < len(lines) and any(line.strip() == "{" for line in lines[i:end]):
                found[match[1]] = lines[i:end + 1]
                i = end
        i += 1
    return found


def normalized(lines, prefix):
    """LINES without comments or layout, with PREFIX's names made neutral."""
    text = re.sub(r"/\*.*?\*/", "", "\n".join(lines), flags=re.S)
    text = re.sub(rf"\b{prefix}_", "BACKEND_", text)
    text = re.sub(rf"\b{prefix.upper()}_", "BACKEND_", text)
    return [" ".join(line.split()) for line in text.split("\n") if line.strip()]


def digest(lines):
    return hashlib.sha256("\n".join(normalized(lines, "pgtk")).encode()).hexdigest()


def pairs(source):
    """Return {pgtk name: (original, copy name, copy, similarity)}."""
    originals = functions(source / "src/pgtkterm.c")
    copies = {}
    for path in sorted((source / "src").glob("wayland*.c")):
        copies.update(functions(path))
    result = {}
    for name, body in copies.items():
        original = originals.get(name.replace("wayland_", "pgtk_", 1))
        if original and name.startswith("wayland_"):
            ratio = difflib.SequenceMatcher(
                None, normalized(original, "pgtk"), normalized(body, "wayland"),
                autojunk=False).ratio()
            result[name.replace("wayland_", "pgtk_", 1)] = (original, name, body, ratio)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--source", type=Path, default=HERE.parents[1],
                        help="the Emacs source tree (default: this one)")
    parser.add_argument("--record", action="store_true",
                        help="record the current originals as ported")
    parser.add_argument("--diff", metavar="FUNCTION",
                        help="show how FUNCTION's copy differs from the original")
    args = parser.parse_args()
    found = pairs(args.source)

    if args.diff:
        name = args.diff.replace("wayland_", "pgtk_", 1)
        if name not in found:
            sys.exit(f"{args.diff}: no PGTK original with a Wayland copy")
        original, copy_name, copy, _ = found[name]
        sys.stdout.write("\n".join(difflib.unified_diff(
            normalized(original, "pgtk"), normalized(copy, "wayland"),
            name, copy_name, lineterm="")) + "\n")
        return

    if args.record:
        record = {name: {"copy": copy_name, "sha256": digest(original),
                         "similarity": round(ratio, 2)}
                  for name, (original, copy_name, _, ratio) in sorted(found.items())
                  if ratio >= TRACKED}
        RECORD.write_text(json.dumps(record, indent=2, sort_keys=True) + "\n",
                          encoding="utf-8")
        print(f"recorded {len(record)} originals in {RECORD.name}")
        return

    if not RECORD.is_file():
        sys.exit(f"{RECORD.name} is missing; run with --record")
    record = json.loads(RECORD.read_text(encoding="utf-8"))
    problems = []
    for name, entry in sorted(record.items()):
        if name not in found:
            problems.append(f"{name}: the original or its copy {entry['copy']} is gone")
        elif digest(found[name][0]) != entry["sha256"]:
            problems.append(f"{name}: changed upstream; port to {entry['copy']} "
                            f"(git log -L ':^{name} [(]:src/pgtkterm.c')")
    for name, (_, copy_name, _, ratio) in sorted(found.items()):
        if name not in record and ratio >= TRACKED:
            problems.append(f"{name}: {copy_name} is a copy that is not recorded")
    for problem in problems:
        print(problem)
    if problems:
        sys.exit(f"{len(problems)} to review; then run {Path(__file__).name} --record")
    print(f"{len(record)} copied PGTK functions are unchanged upstream")


if __name__ == "__main__":
    main()
