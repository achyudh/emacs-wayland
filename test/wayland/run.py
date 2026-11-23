#!/usr/bin/env python3
"""Test Emacs's native Wayland backend against mockcomp and headless sway."""

# This file is part of GNU Emacs, as modified by the native Wayland
# backend fork.  It is free software under
# the GNU General Public License, version 3 or later.

import argparse
from contextlib import ExitStack, contextmanager
import ctypes
from datetime import datetime, timezone
import hashlib
from html import escape
from io import BytesIO
import json
import math
import os
from pathlib import Path
import platform
import random
import re
import shlex
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time

HERE = Path(__file__).resolve().parent
SOURCE = HERE.parent.parent
M0_CASES = (
    "startup", "startup-background", "daemon", "quiet-ping", "busy-lisp",
    "backpressure-input", "write-only", "thread-selection", "idle-loss",
    "redisplay-loss",
    "pre-redisplay-loss", "queued-quit", "thread-loss", "epipe", "overflow",
    "sway-startup", "sway-startup-background", "sway-daemon",
)
STARTUP_BUSY_LISP = """(defvar wayland-test-early-background
  (let ((deadline (+ (float-time) 5)) commits)
    (while (and (zerop (setq commits (plist-get (wayland--metrics)
                                                :background-commits)))
                (< (float-time) deadline)))
    (> commits 0)))"""
SCALE_FIXTURES = ("fractional", "no-viewporter", "no-fractional", "outputs")
SCALE_CASES = tuple(f"scale-{fixture}" for fixture in SCALE_FIXTURES)
SNAPSHOT_CASES = ("pixel-baseline", "glyph-pixels", "interrupted-redraw",
                  "pool-lifetime", "damage-repair", "damage-repair-rgb",
                  "subpixel-redraw", "mid-draw",
                  "mid-redisplay-write") + SCALE_CASES
RENDERING_CASES = SNAPSHOT_CASES + (
    "commit-loss", "pool-budget", "pool-budget-recovery", "frame-states")
PARITY_CASE = "sway-pixel-parity"
PARITY_PHASES = ("hbar", "hollow", "erased", "canvas-initial", "canvas-refresh")
# Every image format on headless sway: pixel parity with PGTK at 100%,
# and reviewed references at the scales.  The GIF steps through its
# frames, each a block of its own color.
IMAGE_PARITY = "sway-image-parity"
IMAGE_PHASES = ("images", "gif-1", "gif-2", "canvas-refresh")
GIF_FRAME_COLORS = ((220, 0, 0), (0, 160, 0), (0, 0, 220))
IMAGE_CANVAS_COLOR = (20, 124, 177)
IMAGE_SCALES = {"sway-image-125": (125, "1200x900"),
                "sway-image-150": (150, "1200x900"),
                "sway-image-200": (200, "1200x900")}
SWAY_SCALES = {"sway-scale-125": (125, "1200x900"),
               "sway-scale-150": (150, "1200x900"),
               "sway-scale-200": (200, "1200x900"),
               "sway-scale-odd": (125, "1024x768")}
INPUT_CASES = ("keyboard", "key-repeat", "seat-changes", "input-queue", "quit",
               "pointer", "pointer-theme", "scroll", "scroll-v5", "scroll-v4",
               "input-method")
CHILD_SCALE_CASES = ("child-no-viewporter", "child-no-fractional")
CHILD_CASES = ("child-frames", "child-hierarchy", "child-no-subcompositor",
               "child-mixed-scale", "child-pointer", "child-drag") + CHILD_SCALE_CASES
SELECTION_CASE = "selection"
SELECTION_SWAY_CASE = "sway-clipboard-pipes"
MENU_CASE = "popup-menu"
ACTIVATION_CASE = "activation"
CHILD_PARITY_CASE = "sway-child-parity"
CHILD_SWAY_SCALES = {CHILD_PARITY_CASE: (100, "1024x768"),
                     "sway-child-125": (125, "1200x900"),
                     "sway-child-150": (150, "1200x900")}
M3_CASES = CHILD_CASES + tuple(CHILD_SWAY_SCALES) + (
    SELECTION_CASE, SELECTION_SWAY_CASE, MENU_CASE, ACTIVATION_CASE)
SEAT_VERSIONS = {"scroll-v5": 5, "scroll-v4": 4}
# The desktop's fonts and text scaling factor, from a settings portal
# the test serves on a private bus.
DESKTOP_CASE = "desktop-settings"
# Soaks: seeded storms on mockcomp, and everyday use on headless sway.
SOAK_STORM, SOAK_SWAY = "soak-storm", "soak-sway"
SOAK_CASES = (SOAK_STORM, SOAK_SWAY)
M4_CASES = (DESKTOP_CASE, "minimal-protocols", *SOAK_CASES)
# Lines in an Emacs log that report a sanitizer finding.
SANITIZER_REPORT = re.compile(r"ERROR: (?:Address|Leak)Sanitizer|runtime error:"
                              r"|SUMMARY: \w+Sanitizer")
# A leak report and its summary.  LeakSanitizer also reports memory
# that only the portable dump's mapping, which it does not scan, refers
# to, and the shared font drivers' objects; a leak fails a scenario only
# when the backend allocated it (`wayland_leaks').
LEAK_LINE = re.compile(r"LeakSanitizer|SUMMARY: AddressSanitizer: \d+ byte\(s\) leaked")
LEAK_BLOCK = re.compile(r"^(?:Direct|Indirect) leak of (\d+) byte\(s\)")
LEAK_FRAME = re.compile(r"^\s+#\d+ 0x[0-9a-f]+ in (\S+) (\S+)")
ALLOCATORS = {"malloc", "calloc", "realloc", "aligned_alloc", "posix_memalign",
              "strdup", "xmalloc", "xzalloc", "xrealloc", "xpalloc", "xnmalloc",
              "xnrealloc", "xstrdup", "xlispstrdup", "lmalloc", "lrealloc",
              "lisp_malloc", "lisp_align_malloc"}


def wayland_leaks(text):
    """The leaks in a LeakSanitizer report whose allocation the backend
    made: the first frame above the allocators is in src/wayland*.c."""
    owned, lines = [], text.splitlines()
    for i, line in enumerate(lines):
        if not (block := LEAK_BLOCK.match(line)):
            continue
        for frame in lines[i + 1:]:
            if not (match := LEAK_FRAME.match(frame)):
                break
            function, location = match[1], match[2]
            if "asan" in location or function in ALLOCATORS:
                continue
            if re.search(r"/src/wayland\w*\.c", location):
                owned.append(f"{block[1]} bytes in {function} {Path(location).name}")
            break
    return owned


# Evdev codes of the letters a to z, for typing in soaks.
KEY_LETTERS = dict(zip("qwertyuiop", range(16, 26))) | dict(zip("asdfghjkl", range(30, 39))) \
    | dict(zip("zxcvbnm", range(44, 51)))
# The capture observer on headless sway at 150%: three runs
# against the reference client, whose p50 may differ by at most
# OBSERVER_SPREAD_MS, and a run typing into Emacs, recorded only.
OBSERVER_CASES = ("observer-reference", "observer-emacs")
TYPING_PERF = "typing-perf"
SCROLL_PERF, SCROLL_CADENCE = "scroll-perf", "scroll-cadence"
IMAGE_SCROLL_PERF = "image-scroll-perf"
SCROLL_PERF_CASES = (SCROLL_PERF, SCROLL_CADENCE, IMAGE_SCROLL_PERF)
STARTUP_PERF, REDRAW_PERF, RESIZE_PERF = "startup-perf", "redraw-perf", "resize-perf"
STARTUP_INIT_PERF = "startup-init-perf"
STARTUP_PERF_CASES = (STARTUP_PERF, STARTUP_INIT_PERF)
SCALE_PERF, CLIPBOARD_PERF = "scale-perf", "clipboard-perf"
CANVAS_PERF, FRAMES_PERF = "canvas-perf", "frames-perf"
PERF_WORKLOADS = (*STARTUP_PERF_CASES, REDRAW_PERF, RESIZE_PERF, SCALE_PERF,
                  CLIPBOARD_PERF, CANVAS_PERF, FRAMES_PERF)
PERF_CASES = (TYPING_PERF, *SCROLL_PERF_CASES, *PERF_WORKLOADS)
# The scroll workload: logical pixels per 120 Hz frame for 10 s.  Each
# backend gets the finger travel that covers that distance: PGTK scrolls
# 10 logical pixels per compositor pixel, since GTK counts a scroll unit
# per 10 and Emacs multiplies by 100; native Wayland scrolls as X does, a
# unit per 10 compositor pixels moving the 2/3 power of the window's
# physical height.
SCROLL_STEP, SCROLL_FRAMES, SCROLL_HZ = 8, 1200, 120


def finger_pixels(backend, window_height, scale):
    """Logical pixels scrolled per compositor pixel of finger travel."""
    if backend == "pgtk":
        return 10
    return window_height ** (2 / 3) / 10 / scale
INPUT_PARITY = "sway-input-parity"
# The parity check's finger strokes: compositor pixels per frame, frames.
FINGER_STEP, FINGER_FRAMES = 2.5, 24
# A real input method: fcitx5 with Hangul on headless sway.
FCITX5_CASE = "sway-fcitx5"
FCITX5_PROFILE = """[Groups/0]
Name=Default
Default Layout=us
DefaultIM=hangul

[Groups/0/Items/0]
Name=keyboard-us
Layout=

[Groups/0/Items/1]
Name=hangul
Layout=

[GroupOrder]
0=Default
"""
OBSERVER_SCALE, OBSERVER_MODE = 150, "1280x720"
OBSERVER_SAMPLES, OBSERVER_RUNS, OBSERVER_SPREAD_MS = 30, 3, 2.0
OBSERVER_TIMEOUT = 90
PERF_MODE = "1200x900@60Hz"
# The default face background perf.el gives the startup scenes.
PERF_STARTUP_MARKER = 0x3a6ea5
PERF_FULLSCREEN = ('for_window [app_id=".+"] fullscreen enable\n'
                   'for_window [class=".+"] fullscreen enable\n')
PERF_FLOATING = ('for_window [app_id=".+"] floating enable\n'
                 'for_window [class=".+"] floating enable\n')
PERF_CLIPBOARD_SIZES = (1024, 1048576, 67108864)
PERF_CLIPBOARD_MIME = "text/plain;charset=utf-8"
PERF_CLIPBOARD_TEXT_TYPES = (PERF_CLIPBOARD_MIME, "UTF8_STRING", "text/plain", "STRING")
PERF_SCALE_MARK = 0x077b3e
INSTRUCTIONS_CASE = "redisplay-instructions"
CASES = (M0_CASES + RENDERING_CASES + (PARITY_CASE, IMAGE_PARITY)
         + tuple(IMAGE_SCALES) + tuple(SWAY_SCALES)
         + INPUT_CASES + OBSERVER_CASES + M3_CASES
         + (FCITX5_CASE, *PERF_CASES, INPUT_PARITY) + M4_CASES
         + (INSTRUCTIONS_CASE,))
# Evdev codes.
BTN_LEFT, BTN_RIGHT, BTN_MIDDLE, BTN_SIDE, BTN_BACK = 0x110, 0x111, 0x112, 0x113, 0x116
CURSOR_DEFAULT, CURSOR_POINTER, CURSOR_TEXT = 1, 4, 9
KEY = {"space": 57, "k": 37, "s": 31, "w": 17, "esc": 1, "backspace": 14, "tab": 15, "return": 28, "ctrl": 29,
       "shift": 42, "alt": 56, "f1": 59, "super": 125, "kp-enter": 96,
       "a": 30, "e": 18, "g": 34, "h": 35, "x": 45, "y": 21, "apostrophe": 40,
       "delete": 111}
# Reviewed counter budgets for the deterministic pool and damage
# fixtures.  --record-budgets writes measured values plus 20%.
BUDGETS = HERE / "budgets.json"
BUDGET_CASES = ("pool-lifetime", "pool-budget", "damage-repair")
BUDGET_MARGIN = 1.2
# Instructions per redisplay of fixed workloads, counted by Callgrind.
# They depend on the compiler flags and on the CPU, so a budget is
# compared only with the build and CPU it was recorded on.  Layout and
# drawing repeat to within 0.2% from run to run.  Restoring the buffer
# Emacs draws into from the one on screen does not: it copies more when
# sway has not yet released the latest buffer.  Typing redraws little,
# so its count varies by about 1%, and its margin is wider.
INSTRUCTIONS_WORKLOADS = ("page", "line", "typing")
INSTRUCTIONS_MODE = "1280x800"
INSTRUCTIONS_MARGINS = {"page": 1.01, "line": 1.01, "typing": 1.03}
INSTRUCTIONS_TIMEOUT = 600
SURVIVOR_CASES = (
    "redisplay-loss", "pre-redisplay-loss", "queued-quit", "thread-loss",
    "epipe", "overflow", "commit-loss",
)


class Failure(Exception):
    """A scenario failed to reach an expected checkpoint."""


def require(condition, explanation):
    if not condition:
        raise Failure(explanation)


def ppm_pixels(contents):
    magic, size, maxval, pixels = contents.split(b"\n", 3)
    width, height = map(int, size.split())
    require(magic == b"P6" and maxval == b"255"
            and len(pixels) == width * height * 3, "invalid grim PPM image")
    return width, height, pixels


def has_rgb(pixels, color):
    return tuple(color) in zip(pixels[0::3], pixels[1::3], pixels[2::3])


def timing_host():
    cpu = Path("/sys/devices/system/cpu")
    fields = ("scaling_driver", "scaling_governor", "scaling_min_freq",
              "scaling_max_freq")
    policies = {}
    for policy in sorted((cpu / "cpufreq").glob("policy*")):
        policies[policy.name] = {
            name: (policy / name).read_text().strip()
            for name in fields if (policy / name).is_file()
        }
    isolated = cpu / "isolated"
    supplies = {}
    for supply in sorted(Path("/sys/class/power_supply").glob("*")):
        supplies[supply.name] = {field: (supply / field).read_text().strip()
                                 for field in ("type", "online", "status")
                                 if (supply / field).is_file()}
    return {"affinity": sorted(os.sched_getaffinity(0)),
            "isolated_cpus": isolated.read_text().strip() if isolated.is_file() else None,
            "frequency_policies": policies, "power_supplies": supplies}


# Build options and features that differ between window systems by
# design; everything else must match between compared builds.
BACKEND_OPTIONS = re.compile(
    r"--(?:with|without)-(?:wayland|pgtk|x|x-toolkit|xinput2|toolkit-scroll-bars"
    r"|gsettings|gconf|xwidgets|xft|xdbe|xpm|xim|cairo|cairo-xcb"
    r"|libotf|m17n-flt)(?:=.*)?$"
    r"|--(?:prefix|exec-prefix|[a-z]*dir)=.*$")
BACKEND_FEATURES = {"WAYLAND", "PGTK", "GTK3", "GSETTINGS", "TOOLKIT_SCROLL_BARS", "X11",
                    "XINPUT2", "XDBE", "XINERAMA", "XRANDR", "XPM", "XFT", "LUCID",
                    "MOTIF", "X_TOOLKIT", "GCONF", "XWIDGETS", "XIM", "OLDXMENU"}


def build_identity(ready):
    """Return what must match between compared builds: the source
    revision, from the repository or from the Nix store path a packaged
    build runs from, and the options, features and native compilation
    other than the window system's."""
    revision = ready.get("commit")
    if not revision:
        match = re.search(r"-emacs[-a-z]*-[0-9.]+\+([0-9a-f]{7,})/", ready.get("directory") or "")
        revision = match and match[1]
    options = sorted(option for option in shlex.split(ready.get("options") or "")
                     if not BACKEND_OPTIONS.match(option))
    features = sorted(set((ready.get("features") or "").split()) - BACKEND_FEATURES)
    return {"revision": revision, "version": ready.get("version"), "options": options,
            "features": features, "native": ready.get("native")}


def same_build(left, right):
    """Whether two build identities come from the same source and settings.
    A short revision matches the full one it starts."""
    a, b = left["revision"], right["revision"]
    return (bool(a and b) and (a.startswith(b) or b.startswith(a))
            and all(left[key] == right[key] for key in ("version", "options", "features",
                                                        "native")))


def newest_tracked(source, paths):
    """The newest modification time among the files Git tracks under
    PATHS in SOURCE."""
    names = subprocess.run(["git", "-C", str(source), "ls-files", "-z", "--", *paths],
                           capture_output=True, text=True).stdout.split("\0")
    return max(((source / name).stat().st_mtime for name in names
                if name and (source / name).is_file()), default=0)


def verify_local_builds(builds, readies, allow_unverified):
    """Check that local builds, which report only the HEAD they were
    dumped at, come from one clean source tree at that HEAD and are
    fresh: each executable newer than the tracked C sources, and each
    portable dump newer than those and the tracked Lisp it preloads.
    Checking a file out, as when reverting it, gives it a new time, so
    a build made before the revert is refused.  BUILDS and READIES map
    each backend to its binary and its ready report.  A binary that
    resolves into the Nix store, as the flake's staged symlinks do, is
    pinned by the revision in its path.  Return whether the local
    builds were verified."""
    local = {backend: Path(binary).resolve() for backend, binary in builds.items()}
    local = {backend: binary for backend, binary in local.items()
             if not binary.is_relative_to("/nix/store")}
    if not local:
        return True
    problems = []
    sources = {readies[backend].get("source") for backend in local}
    if len(sources) != 1 or None in sources:
        problems.append(f"the builds come from different source trees: {sources}")
    else:
        source = Path(sources.pop())
        git = ["git", "-C", str(source)]
        head = subprocess.run(git + ["rev-parse", "HEAD"], capture_output=True,
                              text=True).stdout.strip()
        dirty = subprocess.run(
            git + ["status", "--porcelain", "--untracked-files=no", "--", ".",
                   ":(exclude)admin/notes", ":(exclude)test/wayland"],
            capture_output=True, text=True).stdout.strip()
        if dirty:
            problems.append(f"{source} has uncommitted changes:\n{dirty}")
        executable_sources = newest_tracked(source, ("src", "lib"))
        dump_sources = max(executable_sources, newest_tracked(source, ("lisp",)))
        for backend, binary in local.items():
            if readies[backend].get("commit") != head:
                problems.append(f"the {backend} build was dumped at"
                                f" {readies[backend].get('commit')}, not HEAD {head}")
            dump = binary.with_name("emacs.pdmp")
            if binary.stat().st_mtime < executable_sources:
                problems.append(f"{binary} is older than its C sources; rebuild it")
            if not dump.is_file():
                problems.append(f"{binary} has no portable dump beside it")
            elif dump.stat().st_mtime < dump_sources:
                problems.append(f"{dump} is older than its C or Lisp sources; rebuild it")
    if problems and not allow_unverified:
        raise Failure("cannot verify the builds' source: " + "; ".join(problems))
    return not problems


def trace_samples(samples, entries):
    """Split each observer sample at the backend's trace stages: the
    milliseconds from injection to the key's arrival, its delivery to
    Lisp, the start and end of redisplay, acquiring and repairing a
    buffer and the commit, then to the capture, with commits deferred
    for a frame callback or a buffer and garbage collections between."""
    stages = ("key", "deliver", "begin", "up-to-date", "buffer", "repaired", "commit")
    rows = []
    for sample in samples:
        start, end = sample["injected_ns"], sample["captured_ns"]
        window = [entry for entry in entries if start <= entry[1] <= end]
        row = {"sample": sample["sample"], "latency_ms": sample["latency_ns"] / 1e6}
        last = start
        for stage in stages:
            hit = next((entry for entry in window
                        if entry[0] == stage and entry[1] >= last), None)
            row[stage] = round((hit[1] - start) / 1e6, 3) if hit else None
            if hit:
                last = hit[1]
        row["defers"] = sum(1 for entry in window if entry[0] == "defer")
        row["no_buffers"] = sum(1 for entry in window if entry[0] == "no-buffer")
        row["gcs"] = window[-1][2] - window[0][2] if window else 0
        row["commit_to_capture"] = (round((end - last) / 1e6, 3)
                                    if row["commit"] is not None else None)
        rows.append(row)
    return rows


def present_stages(entries):
    """Summarize content commits and copies in the single-frame trace.
    Commit intervals include work done between their markers.  Copy
    timings also include work before the first marker of a commit."""
    stages = {"draw": [], "to_buffer": [], "repair": [], "commit": []}
    copies = {"restore": []}
    copy_start = {}
    restores = []
    restores_before_clear = []
    marks, gcs = {}, 0
    for name, ns, collections in entries:
        if name.endswith("-start") and name[:-6] in copies:
            copy_start[name[:-6]] = ns
        elif name in copy_start:
            elapsed = ns - copy_start.pop(name)
            copies[name].append(elapsed)
            if name == "restore":
                restores.append(elapsed)
        elif name == "clear":
            restores_before_clear.extend(restores)
            restores.clear()
        elif name == "begin":
            marks = {"begin": (ns, collections)}
        elif name in ("up-to-date", "buffer", "repaired") and marks:
            marks.setdefault(name, (ns, collections))
        elif name == "commit":
            if len(marks) == 4:
                times = [marks[key][0] for key in ("begin", "up-to-date", "buffer",
                                                    "repaired")] + [ns]
                for stage, start, end in zip(stages, times, times[1:]):
                    stages[stage].append(end - start)
                gcs += collections - marks["begin"][1]
            marks = {}
            restores.clear()
    result = {f"{stage}_ms": statistics.median(values) / 1e6
              for stage, values in stages.items() if values}
    return dict(result, commits=len(stages["draw"]), gcs=gcs,
                copies={name: {"count": len(times), "total_ms": sum(times) / 1e6}
                        for name, times in copies.items()},
                restores_before_clear=len(restores_before_clear),
                restore_before_clear_ms=sum(restores_before_clear) / 1e6)


def trace_gaps(times, entries, longest):
    """Describe each interval between captured updates TIMES longer than
    LONGEST milliseconds from the backend's trace ENTRIES: when its last
    commits went out, the redisplays and commits it held, and garbage
    collections."""
    gaps = []
    for start, end in zip(times, times[1:]):
        if (end - start) / 1e6 <= longest:
            continue
        window = [entry for entry in entries if start <= entry[1] <= end]
        begins = [entry[1] for entry in window if entry[0] == "begin"]
        commits = [entry[1] for entry in window if entry[0] == "commit"]
        before = [entry for entry in entries if entry[1] <= start]
        gaps.append({
            "ms": round((end - start) / 1e6, 2),
            "redisplays": len(begins), "commits": len(commits),
            "defers": sum(1 for entry in window if entry[0] == "defer"),
            "no_buffers": sum(1 for entry in window if entry[0] == "no-buffer"),
            "gcs": (window[-1][2] - before[-1][2]) if window and before else 0,
            "longest_without_commit_ms": round(max(
                (b - a) / 1e6 for a, b in zip([start] + commits, commits + [end])), 2),
        })
    return gaps


class Scenario:
    def __init__(self, args, artifacts, name, stack):
        self.args = args
        self.name = name
        self.stack = stack
        self.artifacts = artifacts
        # The observer takes many timed samples.
        timeout = (max(args.timeout, OBSERVER_TIMEOUT)
                   if name in OBSERVER_CASES else args.timeout)
        runs = 3 if args.x_build_dir else 2
        if name == TYPING_PERF:
            timeout = max(timeout, OBSERVER_TIMEOUT + args.perf_pairs
                          * runs * (8 + 2 * args.perf_idle_seconds))
        elif name in SCROLL_PERF_CASES:
            timeout = max(timeout, 30 + args.perf_pairs * runs
                          * (20 + SCROLL_FRAMES / SCROLL_HZ))
        elif name in PERF_WORKLOADS:
            timeout = max(timeout, 30 + args.perf_pairs * runs * 20)
        elif name in SOAK_CASES:
            timeout = max(timeout, 120 + args.soak_seconds * 1.25)
        elif name == INSTRUCTIONS_CASE:
            timeout = max(timeout, INSTRUCTIONS_TIMEOUT)
        self.deadline = time.monotonic() + timeout * args.timeout_scale
        self.data = {}
        self.runtime = Path(stack.enter_context(
            tempfile.TemporaryDirectory(prefix="emacs-wl-", dir="/tmp")))
        # No session bus, so that neither backend reads the desktop's
        # settings; a scenario that needs a bus starts its own.
        # Sanitizer options as etc/DEBUG asks, unless the caller set them.
        self.env = {"ASAN_OPTIONS": "detect_leaks=0,detect_stack_use_after_return=0",
                    "UBSAN_OPTIONS": "print_stacktrace=1", **os.environ}
        self.env.update(XDG_RUNTIME_DIR=str(self.runtime),
                        DBUS_SESSION_BUS_ADDRESS=f"unix:path={self.runtime}/no-bus",
                        NO_AT_BRIDGE="1")
        for var in ("DISPLAY", "WAYLAND_DISPLAY", "WAYLAND_SOCKET", "SWAYSOCK",
                    "WAYLAND_DEBUG", "EMACS_SOCKET_NAME", "EMACS_SERVER_FILE"):
            self.env.pop(var, None)
        if args.fonts:
            self.env.update(self.font_environment())
        self.mocks = []

    def remaining(self):
        remaining = self.deadline - time.monotonic()
        require(remaining > 0, "scenario exceeded its deadline")
        return remaining

    def until(self, check, description, process=None, *, timeout=None):
        deadline = (min(self.deadline, time.monotonic() + timeout)
                    if timeout is not None else self.deadline)
        while True:
            result = check()
            if result:
                return result
            if process is not None and process.poll() is not None:
                raise Failure(f"{description}: process exited {process.returncode}")
            if deadline <= time.monotonic():
                raise Failure(f"timed out waiting for {description}")
            time.sleep(min(0.02, self.remaining()))

    def launch(self, command, log, env, *, stack=None, new_session=False):
        stack = stack if stack is not None else self.stack
        output = stack.enter_context((self.artifacts / log).open("wb"))
        process = subprocess.Popen(command, env=env, stdin=subprocess.DEVNULL,
                                   stdout=output, stderr=subprocess.STDOUT,
                                   start_new_session=new_session)
        stack.callback(self.stop, process)
        return process

    @staticmethod
    def stop(process):
        if process.poll() is None:
            process.terminate()
            try:
                process.wait(1)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()

    def finish(self, process, expected=0):
        status = process.wait(self.remaining())
        require(status == expected, f"expected process exit {expected}, got {status}")

    def execute(self, command, env=None):
        result = subprocess.run(command, env=self.env if env is None else env,
                                timeout=self.remaining(), capture_output=True,
                                text=True, stdin=subprocess.DEVNULL)
        require(result.returncode == 0,
                f"{command[0]} exited {result.returncode}: {result.stderr.strip()}")
        return result.stdout.strip()

    def control(self, command, name="primary"):
        with socket.socket(socket.AF_UNIX) as connection:
            connection.settimeout(self.remaining())
            connection.connect(str(self.runtime / f"{name}.ctl"))
            connection.sendall((command + "\n").encode())
            with connection.makefile(encoding="utf-8") as stream:
                reply = stream.readline().rstrip("\n")
        require(reply != "" and not reply.startswith("error"),
                f"mockcomp {command}: {reply or 'no reply'}")
        return reply

    def status(self, name="primary"):
        status = json.loads(self.control("status", name))
        require(status["violation_total"] == 0, f"mockcomp violations: {status}")
        return status

    def mock(self, name="primary"):
        args = [self.args.mockcomp, "--socket", name,
                "--control", self.runtime / f"{name}.ctl",
                "--trace", self.artifacts / f"{name}.trace"]
        if self.name in SNAPSHOT_CASES:
            snapshots = self.artifacts / "submitted"
            snapshots.mkdir()
            args.extend(("--snapshots", snapshots))
        if self.name in SCALE_CASES:
            args.extend(("--scale-fixture", self.name.removeprefix("scale-")))
        if self.name in CHILD_SCALE_CASES:
            args.extend(("--scale-fixture", self.name.removeprefix("child-")))
        if self.name == "child-mixed-scale":
            args.extend(("--scale-fixture", "fractional"))
        if self.name == "pointer-theme":
            args.append("--no-cursor-shape")
        if self.name == "child-no-subcompositor":
            args.append("--no-subcompositor")
        if self.name == ACTIVATION_CASE:
            args.append("--activation-fixture")
        if self.name == "minimal-protocols":
            args.append("--minimal-protocols")
        if self.name in SEAT_VERSIONS:
            args.extend(("--seat-version", str(SEAT_VERSIONS[self.name])))
        process = self.launch(args,
                              f"{name}.log", self.env)
        self.until((self.runtime / f"{name}.ctl").is_socket, f"mockcomp {name}", process)
        self.mocks.append((name, process))
        return name

    def finish_mocks(self):
        for name, process in self.mocks:
            self.data[f"mock-{name}"] = self.status(name)
            self.control("quit", name)
            self.finish(process)

    def sway(self, extra_config="", mode="1024x768", scale=100,
             renderer="pixman", render_node=None):
        display, ipc, _ = self.start_sway(self.runtime, "sway", extra_config,
                                          mode, scale, renderer, render_node)
        return display.name, ipc

    def start_sway(self, runtime, name, extra_config, mode, scale, renderer,
                   render_node, *, xwayland=False, stack=None):
        require(self.args.sway and self.args.swaymsg, "sway and swaymsg are required")
        config = self.artifacts / f"{name}.conf"
        config.write_text(("xwayland force\n" if xwayland else "xwayland disable\n") +
                          f"output HEADLESS-1 resolution {mode} scale {scale / 100}\n"
                          + extra_config, encoding="utf-8")
        env = dict(
            self.env, XDG_RUNTIME_DIR=str(runtime),
            WLR_BACKENDS="headless", WLR_HEADLESS_OUTPUTS="1",
            WLR_RENDERER=renderer)
        if self.name == FRAMES_PERF:
            env["WLR_HEADLESS_OUTPUTS"] = "2"
        if render_node:
            env["WLR_RENDER_DRM_DEVICE"] = str(render_node)
        command = [self.args.sway, "-c", config]
        if self.name in PERF_CASES:
            command.append("-V")
        process = self.launch(command, f"{name}.log", env, stack=stack,
                              new_session=xwayland)
        self.until(lambda: any(runtime.glob("sway-ipc*.sock"))
                   and any(p.is_socket() for p in runtime.glob("wayland-*")),
                   "headless sway sockets", process)
        ipc = next(runtime.glob("sway-ipc*.sock"))
        display = next(p for p in runtime.glob("wayland-*") if p.is_socket())
        return display, ipc, process

    def sway_mapped(self, ipc, process):
        def has_window(nodes):
            return any(node.get("app_id") == "emacs"
                       or has_window(node.get("nodes", []) + node.get("floating_nodes", []))
                       for node in nodes)

        def check():
            tree = self.execute([self.args.swaymsg, "-s", ipc, "-t", "get_tree"])
            (self.artifacts / "sway-tree.json").write_text(tree + "\n", encoding="utf-8")
            return has_window([json.loads(tree)])

        self.until(check, "a mapped Emacs toplevel in sway", process)
        require("wl_surface" in self.emacs_log(), "missing Wayland protocol trace")

    def emacs_log(self, log="emacs.log"):
        return (self.artifacts / log).read_text(encoding="utf-8", errors="replace")

    def protocol_errors(self, log="emacs.log"):
        return "\n".join(re.findall(r"^.*wl_display(?:@|#)\d+\.error\(.*$",
                                    self.emacs_log(log), re.MULTILINE))

    def session_bus(self):
        """Start a private session bus and return its address.  It has
        no service directories, so nothing on it is activated: the
        standard session configuration would start the real portal."""
        config = self.artifacts / "bus.conf"
        config.write_text(
            '<!DOCTYPE busconfig PUBLIC "-//freedesktop//DTD D-Bus Bus Configuration'
            ' 1.0//EN" "http://www.freedesktop.org/standards/dbus/1.0/busconfig.dtd">\n'
            "<busconfig><type>session</type>"
            f"<listen>unix:path={self.runtime}/test-bus</listen>"
            '<policy context="default"><allow send_destination="*" eavesdrop="true"/>'
            '<allow receive_sender="*"/><allow eavesdrop="true"/><allow own="*"/>'
            "</policy></busconfig>\n", encoding="utf-8")
        process = subprocess.Popen(["dbus-daemon", f"--config-file={config}", "--nofork",
                                    "--print-address=1"],
                                   env=self.env, stdout=subprocess.PIPE, text=True)
        self.stack.callback(self.stop, process)
        address = process.stdout.readline().strip()
        require(address, "dbus-daemon did not start")
        return address

    def gui(self, display, emacs=None, log="emacs.log", extra_env=None, wrapper=None):
        listener = self.stack.enter_context(socket.socket(socket.AF_UNIX))
        endpoint = self.runtime / f"{log}.sock"
        listener.bind(str(endpoint))
        listener.listen()
        env = dict(self.env, WAYLAND_DISPLAY=display, WAYLAND_DEBUG="client",
                   EMACS_WAYLAND_TEST=self.name.removeprefix("sway-"),
                   EMACS_WAYLAND_TEST_SOCKET=str(endpoint),
                   EMACS_WAYLAND_TEST_SURVIVOR="survivor",
                   EMACS_WAYLAND_TEST_RESUME=str(self.runtime / "resume"))
        if extra_env:
            env.update(extra_env)
        args = [emacs or self.args.emacs]
        if self.name == STARTUP_INIT_PERF:
            args.extend(("--no-site-file", "--no-splash", "--debug-init",
                         "--init-directory", self.perf_init))
        elif self.name.removeprefix("sway-") == "startup-background":
            # Stay in Lisp in the init file, as a long one does, until
            # the first configure is answered.  The frame maps before
            # the init file loads.
            init = self.runtime / "init"
            init.mkdir()
            (init / "init.el").write_text(STARTUP_BUSY_LISP + "\n", encoding="utf-8")
            args.extend(("--no-site-file", "--no-site-lisp", "--no-x-resources",
                         "--no-splash", "--init-directory", init))
        else:
            args.append("-Q")
        args.extend(("--load", HERE / "m0.el"))
        if wrapper:
            args[:0] = wrapper
        elif self.args.wrapper and not emacs:
            args[:0] = shlex.split(self.args.wrapper)
        if (self.name in RENDERING_CASES + INPUT_CASES + OBSERVER_CASES + M3_CASES
                or self.name in (PARITY_CASE, FCITX5_CASE, *PERF_CASES, INPUT_PARITY,
                                 *SWAY_SCALES, IMAGE_PARITY, *IMAGE_SCALES)):
            args.extend(("--load", HERE / "m1.el"))
        if self.name in INPUT_CASES + OBSERVER_CASES + (FCITX5_CASE, *PERF_CASES,
                                                        INPUT_PARITY):
            args.extend(("--load", HERE / "m2.el"))
        if self.name in (*PERF_WORKLOADS, IMAGE_SCROLL_PERF):
            args.extend(("--load", HERE / "perf.el"))
        if self.name in M3_CASES:
            args.extend(("--load", HERE / "m3.el"))
        if self.name in M4_CASES:
            args.extend(("--load", HERE / "m1.el", "--load", HERE / "m4.el"))
        if self.name == INSTRUCTIONS_CASE:
            args.extend(("--load", HERE / "m2.el", "--load", HERE / "instructions.el"))
        if self.name in SNAPSHOT_CASES or self.name in SWAY_SCALES:
            canvas = self.artifacts / "canvas"
            canvas.mkdir()
            env["EMACS_WAYLAND_TEST_DUMP"] = str(canvas)
        args.extend(("--eval", "(run-at-time 0 nil #'wayland-test-run)"))
        if self.name in PERF_CASES:
            self.perf_started_ns = time.monotonic_ns()
        process = self.launch(args, log, env)
        listener.setblocking(False)

        def accept():
            try:
                return listener.accept()
            except BlockingIOError:
                return None

        connection, _ = self.until(accept, f"{log} to connect", process)
        self.connection = self.stack.enter_context(connection)
        self.input = self.stack.enter_context(connection.makefile(encoding="utf-8"))
        self.phase("ready")
        return process

    def phase(self, expected, *, timeout=None):
        remaining = self.remaining()
        self.connection.settimeout(min(remaining, timeout) if timeout else remaining)
        line = self.input.readline()
        require(line, f"Emacs exited before checkpoint {expected}")
        message = json.loads(line)
        require(message["phase"] == expected,
                f"expected checkpoint {expected}, got {message}")
        if message["data"]:
            self.data[expected] = message["data"]
        return message["data"]

    def send(self, command):
        self.connection.settimeout(self.remaining())
        self.connection.sendall((command + "\n").encode())

    def daemon(self, display, ipc=None):
        name = "wayland-m0"
        process = self.launch([self.args.emacs, "-Q", "--fg-daemon=" + name],
                              "emacs.log", dict(self.env, WAYLAND_DEBUG="client"))
        server = self.runtime / "emacs" / name
        self.until(server.is_socket, "Emacs daemon socket", process)
        client = [self.args.emacsclient, "-s", server]
        require(self.execute(client + ["-e", '''(and (daemonp)
                    (null (getenv "DISPLAY")) (null (getenv "WAYLAND_DISPLAY"))
                    (null (x-display-list)))''']) == "t",
                "daemon did not start without a display")
        self.execute(client + ["-c", "-n"], dict(self.env, WAYLAND_DISPLAY=display))
        require(self.execute(client + ["-e", f'''(catch 'found
            (dolist (frame (frame-list))
              (when (and (eq (window-system frame) 'wayland)
                         (equal (frame-parameter frame 'display) {json.dumps(display)}))
                (throw 'found t))))''']) == "t",
                "emacsclient did not create a frame on the requested display")
        if ipc is None:
            self.until(lambda: self.status()["mapped"] == 1, "daemon frame map", process)
        else:
            self.sway_mapped(ipc, process)
        self.execute(client + ["-e", "(kill-emacs)"])
        self.finish(process)

    def selection_request(self, process, mime=""):
        self.control(f"request-selection {mime}".rstrip())
        self.until(lambda: self.control("selection-result") == "ok",
                   "the selection pipe payload", process)

    def backpressure(self, process):
        with_input = self.name == "backpressure-input"
        if with_input:
            self.until(lambda: self.status()["selections_set"] == 1,
                       "test selection published", process)
        initial = self.status()["commits"]
        self.control("stall on")
        self.send("fill")
        self.phase("pending")
        self.control("configure 800 600")
        if with_input:
            self.control("ping")
            self.control("key 30 press")
            self.control("key 30 release")
            self.selection_request(process)
        self.send("draw")
        settled = self.phase("held")
        status = self.status()
        require(not status["acked_latest"] and status["commits"] == initial,
                "configure was presented before the socket drained")
        require(status["pongs"] == 0, "mockcomp consumed requests while stalled")
        self.control("stall off")
        self.until(lambda: self.status()["acked_latest"], "held configure commit", process)
        status = self.status()
        require(status["buffer_size"] == settled and status["commits"] > initial,
                "resumed presentation did not commit the new buffer")
        if with_input:
            self.until(lambda: self.status()["pongs"] == 1, "deferred pong", process)
        self.send("done")
        self.phase("metrics")

    def snapshot(self, sequence, process, surface=None):
        if surface is None:
            surface, _ = self.data["ready"]
        name = f"surface-{surface}-{sequence}.png"
        canvas = self.artifacts / "canvas" / name
        submitted = self.artifacts / "submitted" / name

        def captured():
            # Status follows the mock's write, but Emacs may still be
            # writing its own PNG.  Wait for the complete IEND chunk.
            if self.status()["mapped"] > 0 and submitted.is_file() and canvas.is_file():
                contents = canvas.read_bytes()
                if contents.endswith(b"\0\0\0\0IEND\xae\x42\x60\x82"):
                    return contents

        dumped = self.until(captured, f"the snapshot of {name}", process)
        contents = submitted.read_bytes()
        require(contents == dumped, f"canvas differs for {name}")
        self.data.setdefault("images", []).append(name)
        return contents

    def pixel_baseline(self, process):
        before = self.status()["content_commits"]
        previous = None
        for sequence, command in enumerate(("first", "second"), before + 1):
            self.send(command)
            contents = self.snapshot(sequence, process)
            require(contents != previous, "successive paints produced the same image")
            previous = contents
        self.send("done")

    def interrupted_redraw(self, process):
        initial = self.status()
        self.send("interrupt")
        self.phase("interrupted")
        self.control("frame-callbacks on")
        self.until(lambda: self.status()["frames_done"] > initial["frames_done"],
                   "release of the frame callback", process)
        width, height = initial["buffer_size"]
        self.control(f"configure {width} {height}")
        self.control("ping")
        self.until(lambda: self.status()["pongs"] == 1,
                   "dispatch after the interrupted redraw", process)
        blocked = self.status()
        require(blocked["commits"] == initial["commits"] and not blocked["acked_latest"],
                "Emacs committed the interrupted redraw")
        self.send("redraw")
        self.snapshot(initial["content_commits"] + 1, process)
        require(self.status()["acked_latest"], "redraw did not acknowledge the configure")
        self.send("done")

    def m1_step(self, command):
        self.send(command)
        state = dict(zip(("pool", "retired", "charged", "budget", "repair",
                          "full", "age", "scroll", "ack_only", "release_ns",
                          "discards"),
                         self.phase("state")))
        counters = self.data.setdefault("counters", {"peak_pool_bytes": 0})
        counters.update(repair_bytes=state["repair"], full_repairs=state["full"],
                        scroll_bytes=state["scroll"],
                        peak_pool_bytes=max(counters["peak_pool_bytes"],
                                            state["pool"]))
        if state["release_ns"]:
            # Elapsed time is a diagnostic, never a budget.
            self.data.setdefault("release_latency_ns", []).append(state["release_ns"])
        return state

    def check_budgets(self):
        """Compare this fixture's counters with its reviewed budgets."""
        counters = self.data["counters"]
        budgets = json.loads(BUDGETS.read_text()) if BUDGETS.is_file() else {}
        if self.args.record_budgets:
            budgets[self.name] = {key: math.ceil(value * BUDGET_MARGIN)
                                  for key, value in sorted(counters.items())}
            BUDGETS.write_text(json.dumps(budgets, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
            return
        limits = budgets.get(self.name)
        require(limits, f"no counter budget for {self.name}; "
                "review the output of --record-budgets")
        over = [f"{key} {counters.get(key)} > {limit}"
                for key, limit in limits.items() if counters.get(key, 0) > limit]
        require(not over, "counter budget exceeded: " + ", ".join(over))

    def redisplay_instructions(self):
        """Count the instructions of fixed redisplay workloads under
        Callgrind on headless sway, and compare them with the budget
        recorded for this build configuration."""
        valgrind = shutil.which("valgrind")
        require(valgrind, "valgrind is required")
        require(self.args.fonts, "--fonts is required")
        display, _ = self.sway('for_window [app_id=".+"] fullscreen enable\n',
                               mode=INSTRUCTIONS_MODE)
        env = dict(self.scene_environment(), WAYLAND_DEBUG="",
                   EMACS_WAYLAND_TEST_SOURCE=str(SOURCE / "src/xdisp.c"))
        counts = self.data.setdefault("instructions", {})
        for workload in INSTRUCTIONS_WORKLOADS:
            out = self.artifacts / f"callgrind-{workload}.out"
            # Collect only inside Fredisplay, which the workload alone calls.
            process = self.gui(
                display, log=f"{workload}.log",
                extra_env=dict(env, EMACS_WAYLAND_TEST_WORKLOAD=workload),
                wrapper=[valgrind, "--tool=callgrind", "--toggle-collect=Fredisplay",
                         f"--callgrind-out-file={out}"])
            ready = self.data.pop("ready")
            redisplays = self.phase("done")
            self.finish(process)
            totals = re.search(r"^totals: (\d+)", out.read_text(), re.M)
            require(totals and redisplays, f"no instruction count for {workload}")
            counts[workload] = int(totals[1]) // redisplays
        # What the counts depend on besides the source: the configure
        # flags and CFLAGS (the other recorded variables are paths),
        # and the CPU, by which the libraries choose their routines.
        identity = build_identity(ready)
        cpu = re.search(r"^model name\s*:\s*(.*)$",
                        Path("/proc/cpuinfo").read_text(), re.M)
        build = {"version": identity["version"], "native": identity["native"],
                 "options": [option for option in identity["options"]
                             if option.startswith(("--", "CFLAGS="))],
                 "cpu": cpu[1] if cpu else platform.machine()}
        self.data.update(build=build, frame=[ready["width"], ready["height"]])
        budgets = json.loads(BUDGETS.read_text()) if BUDGETS.is_file() else {}
        if self.args.record_budgets:
            budgets[self.name] = {"build": build, "limits": {
                key: math.ceil(value * INSTRUCTIONS_MARGINS[key])
                for key, value in sorted(counts.items())}}
            BUDGETS.write_text(json.dumps(budgets, indent=2, sort_keys=True) + "\n",
                               encoding="utf-8")
            return
        budget = budgets.get(self.name)
        require(budget, f"no instruction budget for {self.name}; "
                "review the output of --record-budgets")
        self.data["compared"] = budget["build"] == build
        if not self.data["compared"]:
            print(f"NOTE {self.name}: the budget is for another build or CPU;"
                  " counts are reported, not compared", flush=True)
            return
        over = [f"{key} {counts[key]} > {limit}"
                for key, limit in budget["limits"].items() if counts[key] > limit]
        require(not over, "instruction budget exceeded: " + ", ".join(over))

    def sync_mock(self, process):
        before = self.status()["pongs"]
        self.control("ping")
        self.until(lambda: self.status()["pongs"] > before, "Wayland pong", process)

    def quiesce(self, process, interval=0.15):
        """Wait until no surface has committed for INTERVAL seconds.
        Startup redraws, such as the one after the fixture's font change,
        land at times that depend on when the input hook runs."""
        def commits():
            self.sync_mock(process)
            return [(surface["id"], surface["content_commits"])
                    for surface in self.status()["toplevels"]]

        previous = commits()
        while True:
            time.sleep(interval)
            current = commits()
            if current == previous:
                return
            require(self.remaining() > interval, "surfaces never stopped committing")
            previous = current

    def fill_pool(self, process):
        """Leave the compositor holding exactly the live pool's three
        buffers, and return them."""
        surface, _ = self.data["ready"]
        self.quiesce(process)
        # Startup commits land in the pool of whichever size was current
        # when they happened.  Release them all, so the compositor holds
        # only the three buffers committed below, all from the live pool.
        for sid, commit in self.status()["held"]:
            if sid == surface:
                self.control(f"release {surface} {commit}")
        self.sync_mock(process)
        for row in range(3):
            sequence = self.status()["content_commits"] + 1
            self.m1_step(f"edit {row}")
            self.snapshot(sequence, process)
        held = sorted(commit for sid, commit in self.status()["held"] if sid == surface)
        require(len(held) == 3, f"compositor holds {held}, not three buffers")
        return surface, held

    def resize(self, sizes, process):
        self.control(f"configure {sizes}")
        self.sync_mock(process)
        return self.m1_step("resize")

    def pool_lifetime(self, process):
        surface, held = self.fill_pool(process)
        before = self.status()["buffer_size"]

        # A configure that keeps the size is acked while all three
        # buffers are held, by a commit that attaches nothing.
        commits = self.status()["content_commits"]
        acks = self.m1_step("state")["ack_only"]
        state = self.resize(f"{before[0]} {before[1]}", process)
        status = self.status()
        require(status["acked_latest"],
                "same-size configure waited for a buffer release")
        require(status["content_commits"] == commits
                and status["buffer_size"] == before,
                "same-size ack attached a new buffer")
        require(sorted(commit for sid, commit in status["held"]
                       if sid == surface) == held,
                "same-size ack changed the held buffers")
        require(state["ack_only"] == acks + 1,
                "same-size ack was not a bufferless commit")
        self.data["same-size-ack"] = [before, state["ack_only"]]
        self.m1_step("fail-pool")
        self.resize("1024 768", process)
        status = self.status()
        require(status["buffer_size"] == before and not status["acked_latest"],
                "pool allocation failure replaced the visible image")
        self.m1_step("restore")
        sequence = status["content_commits"] + 1
        self.snapshot(sequence, process)
        status = self.status()
        require(status["acked_latest"] and status["buffer_size"] == [1024, 768],
                "replacement did not acknowledge the latest configure")
        require(all([surface, commit] in status["held"] for commit in held),
                "replacement needed a release from the original pool")
        state = self.m1_step("state")
        original_bytes = before[0] * before[1] * 12
        require(state["retired"] == original_bytes and state["charged"] == 0,
                "first retired pool was not covered by the resize reserve")

        # Mix large and small pools, then hold a sequence of large pools
        # until admission is deferred. Later configures replace that size.
        self.data["pool-sizes"] = []
        for width, height in [(480, 360), (1800, 1000), (1810, 1010),
                              (1820, 1020), (1830, 1030), (1840, 1040)]:
            stable = self.status()["content_commits"]
            self.resize(f"{width} 0 0 {height} {width} {height}", process)
            self.sync_mock(process)
            status = self.status()
            state = self.m1_step("state")
            self.data["pool-sizes"].append([width, height, status["acked_latest"], state])
            require(state["charged"] <= state["budget"], "retired pool budget exceeded")
            if not status["acked_latest"]:
                require(status["content_commits"] == stable, "unacked resize committed")
                break
            self.snapshot(stable + 1, process)
            require(status["buffer_size"] == [width, height], "configure burst did not merge")
        else:
            raise Failure("resize storm never reached the retired pool budget")

        self.resize("720 520 840 640", process)
        require(not self.status()["acked_latest"], "budget-blocked configure was acked")
        # The frame's drawing was discarded while it had no buffer.
        # The releases let the next redisplay draw it at the last size.
        self.control("hold-buffers off")
        self.sync_mock(process)
        self.m1_step("state")
        self.snapshot(stable + 1, process)
        status = self.status()
        require(status["acked_latest"] and status["buffer_size"] == [840, 640],
                "release did not resume the last configure")
        require(self.m1_step("state")["retired"] == 0, "retired pools did not drain")

        self.m1_step("hide")
        self.sync_mock(process)
        require(self.status()["mapped"] == 0, "frame remained mapped after hiding")
        self.m1_step("show")
        self.sync_mock(process)
        self.m1_step("resize")
        self.snapshot(stable + 2, process)
        require(self.status()["acked_latest"], "remap did not acknowledge its configure")
        self.send("done")

    def damage_repair(self, process):
        surface, held = self.fill_pool(process)
        oldest, second, latest = held
        sequence = self.status()["content_commits"]
        self.control(f"release {surface} {second}")
        initial = self.m1_step("state")
        width, height = self.status()["buffer_size"]
        self.m1_step("edit 6")
        sequence += 1
        self.snapshot(sequence, process)
        state = self.m1_step("state")
        require(state["age"] == sequence - second and state["full"] == initial["full"],
                "released buffer was not repaired from damage history")
        require(0 < state["repair"] - initial["repair"] < width * height * 4,
                "small edit did not use partial repair")
        damaged = self.status()["toplevels"][0]["damage_pixels"]
        require(0 < damaged < width * height, "small edit damaged the whole surface")

        # Keep the oldest slot held beyond the eight-entry history.
        for row in range(7, 16):
            self.control(f"release {surface} {sequence}")
            self.m1_step(f"edit {row}")
            sequence += 1
            self.snapshot(sequence, process)
        self.control(f"release {surface} {oldest}")
        before = self.m1_step("state")
        self.m1_step("edit 4")
        sequence += 1
        self.snapshot(sequence, process)
        after = self.m1_step("state")
        require(after["age"] == sequence - oldest and after["full"] == before["full"] + 1,
                "expired history did not trigger full repair")
        require(after["repair"] - before["repair"] == width * height * 4,
                "expired buffer was not repaired over its entire image")

        self.control(f"release {surface} {latest}")
        self.m1_step("scroll")
        scrolled = self.snapshot(sequence + 1, process)
        if self.name == "damage-repair-rgb":
            from PIL import Image, ImageChops

            with Image.open(BytesIO(scrolled)) as image:
                red, _, blue = image.convert("RGB").split()
                require(ImageChops.difference(red, blue).getbbox(),
                        "subpixel antialiasing did not render colored pixels")
        before_redraw = self.m1_step("state")
        require(before_redraw["scroll"] > after["scroll"],
                "scroll did not exercise the row copy")

        # All three buffers are held again, so a complete redraw has
        # no buffer to draw into.  It is discarded: nothing is copied
        # and nothing is committed.
        redrawn = self.m1_step("redraw")
        require(redrawn["discards"] == before_redraw["discards"] + 1,
                "a redraw with every buffer held was not discarded")
        require(redrawn["repair"] == before_redraw["repair"]
                and redrawn["full"] == before_redraw["full"],
                "a redraw with every buffer held copied pixels")
        require(self.status()["content_commits"] == sequence + 1,
                "redraw committed while all buffers were held")
        # After a release, the next redisplay draws the frame afresh
        # into the released buffer, brought up to date at most once.
        self.control(f"release {surface} {sequence}")
        self.sync_mock(process)
        recovered = self.m1_step("state")
        require(self.snapshot(sequence + 2, process) == scrolled,
                "the redraw after a release changed the frame")
        require(recovered["repair"] - redrawn["repair"] <= width * height * 4,
                "the redraw after a release copied more than one image")
        self.control("hold-buffers off")
        require(self.status()["held_checks"] >= 12, "held buffer bytes were not checked")
        self.send("done")

    def subpixel_redraw(self, process):
        """Swap words beside unchanged text under RGB subpixel
        antialiasing.  The frame must not change from round to round,
        as pixels left behind darken, and must stay close to a full
        redraw, which a missing or stale filter pixel is not."""
        from PIL import Image, ImageChops

        def frame():
            self.quiesce(process)
            sequence = self.status()["content_commits"]
            with Image.open(BytesIO(self.snapshot(sequence, process))) as image:
                return image.convert("RGB")

        def rounds(count):
            for _ in range(2 * count):
                self.m1_step("swap")
            return frame()

        early = rounds(2)
        red, _, blue = early.split()
        require(ImageChops.difference(red, blue).getbbox(),
                "subpixel antialiasing did not render colored pixels")
        late = rounds(10)
        require(not ImageChops.difference(early, late).getbbox(),
                "pixels changed over repeated redraws of the same text")
        self.m1_step("redraw")
        full = frame()
        largest = max(high for _, high in ImageChops.difference(late, full).getextrema())
        self.data["subpixel"] = {"largest_difference": largest}
        require(largest <= 32,
                f"drawing in parts differs from a full redraw by up to {largest}")
        self.send("done")

    def pool_budget(self, process):
        self.m1_step("add-frames")
        self.until(lambda: self.status()["mapped"] == 3, "three mapped frames", process)
        self.control("hold-buffers off")

        def commits():
            return {surface["id"]: surface["content_commits"]
                    for surface in self.status()["toplevels"]}

        generations = []

        def settle(width, height):
            """Resize every frame and return, per frame, the held buffers
            of this size committed since the resize began.  A redraw at
            the previous size can still land after the configure; such
            buffers join the previous generation, which shares its pool."""
            before = commits()
            self.resize(f"{width} {height}", process)
            self.until(lambda: all(surface["acked_latest"]
                                  and surface["buffer_size"] == [width, height]
                                  for surface in self.status()["toplevels"]),
                       "all three replacement buffers", process)
            self.quiesce(process)
            held = self.status()["held_sizes"]
            generation = {surface: [] for surface in before}
            previous = generations[-1] if generations else None
            for sid, commit, w, h in held:
                if sid not in before or commit <= before[sid]:
                    continue
                if [w, h] == [width, height]:
                    generation[sid].append(commit)
                elif previous and [w, h] == previous[0]:
                    previous[1][sid].append(commit)
            generations.append(([width, height], generation))
            self.data.setdefault("generations", []).append(
                [width, height, {str(k): v for k, v in generation.items()}])
            return generation

        def release(generation):
            for surface, held in generation.items():
                for commit in held:
                    self.control(f"release {surface} {commit}")
            self.sync_mock(process)

        first = settle(1920, 1080)
        require(self.m1_step("state")["retired"] == 0, "old pools did not drain")
        self.control("hold-buffers on")
        reserved = settle(1920, 1079)
        front = settle(1920, 1080)
        limit = self.m1_step("state")
        require(limit["charged"] == limit["budget"], "test did not fill display budget")
        if self.name == "pool-budget-recovery":
            waiter, _ = self.data["ready"]
            donor = next(surface for surface in first if surface != waiter)
            require(first[donor] and front[waiter], "test did not hold both pools")
            self.control(f"configure-surface {waiter} 1870 1030")
            self.sync_mock(process)
            blocked = self.m1_step("state")
            require(blocked["discards"] > limit["discards"],
                    "a budget-blocked resize did not discard drawing")
            before = commits()[waiter]

            def resized():
                return any(surface["id"] == waiter and surface["acked_latest"]
                           and surface["buffer_size"] == [1870, 1030]
                           and surface["content_commits"] > before
                           for surface in self.status()["toplevels"])

            require(not resized(), "resize completed before space became available")
            # Release a charged pool from another frame.  Leave every
            # buffer in the waiting frame held, with Lisp idle.
            self.send("idle")
            self.phase("idle")
            for commit in first[donor]:
                self.control(f"release {donor} {commit}")
            self.until(resized, "resize after another frame released a charged pool",
                       process, timeout=5)
            held = self.status()["held"]
            require(all([waiter, commit] in held for commit in front[waiter]),
                    "the waiting frame needed its own buffer release")
            self.data["pool-recovery"] = {"waiting_surface": waiter,
                                          "releasing_surface": donor}
            self.control("hold-buffers off")
            self.send("done")
            self.tap("a")
            return
        release(reserved)
        replacement = settle(1880, 1040)
        status = self.status()
        for surface, held in front.items():
            require(replacement[surface]
                    and all([surface, commit] in status["held"] for commit in held),
                    "a frame waited for its old front buffer at the budget limit")
        release(front)
        state = self.m1_step("state")
        require(state["charged"] == state["budget"], "resize changed the charged budget")
        release(first)
        release(replacement)
        require(self.m1_step("state")["retired"] == 0, "old pools stayed allocated")
        self.data["budget-at-limit"] = limit
        self.send("done")

    def mid_draw(self, process):
        surface, _ = self.data["ready"]
        initial = self.status()
        self.send("draw")
        self.phase("drawing-next")
        held = next(commit for sid, commit in initial["held"] if sid == surface)
        self.control(f"release {surface} {held}")
        self.control("configure 820 640 800 600")
        (self.runtime / "resume").touch()
        self.phase("drew")
        require("handled release and configure during drawing" in self.emacs_log(),
                "release and configure were not dispatched by the draw probe")
        self.send("redraw")
        self.snapshot(initial["content_commits"] + 1, process)
        status = self.status()
        require(status["acked_latest"] and status["buffer_size"] == [800, 600],
                "first drawable frame did not settle the latest configure")
        self.send("done")

    def mid_redisplay_write(self, process):
        initial = self.status()["content_commits"]
        self.control("stall on")
        self.send("draw")
        self.phase("pending")
        self.control("stall off")
        self.phase("resumed")
        require(self.status()["content_commits"] == initial,
                "writable callback committed while redisplay still blocked flips")
        self.send("leave")
        self.snapshot(initial + 1, process)
        self.send("done")

    def tap(self, key, *modifiers):
        """Press and release KEY with MODIFIERS held, by name."""
        for modifier in modifiers:
            self.control(f"key {KEY[modifier]} press")
        self.control(f"key {KEY[key]} press")
        self.control(f"key {KEY[key]} release")
        for modifier in reversed(modifiers):
            self.control(f"key {KEY[modifier]} release")

    def read_keys(self, count=1):
        self.send(f"read {count}")
        return self.phase("keys")

    def focus_state(self):
        self.send("focus-state")
        return self.phase("focus")

    def mouse_state(self):
        self.send("mouse-state")
        return self.phase("mouse")

    def keyboard(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        require(self.focus_state() == "t", "keyboard enter did not focus the frame")
        cases = [(("a",), "a"), (("a", "shift"), "A"), (("a", "ctrl"), "C-a"),
                 (("h", "ctrl"), "C-h"), (("delete",), "<delete>"),
                 (("x", "alt"), "M-x"), (("a", "super"), "s-a"),
                 (("a", "ctrl", "alt"), "C-M-a"), (("f1",), "<f1>"),
                 (("f1", "shift"), "S-<f1>"), (("return",), "<return>"),
                 (("backspace",), "<backspace>"), (("tab",), "<tab>"),
                 (("esc",), "<escape>"), (("kp-enter",), "<kp-enter>")]
        results = self.data["keys"] = []
        for (key, *modifiers), expected in cases:
            self.tap(key, *modifiers)
            got = self.read_keys()
            results.append([key, modifiers, got])
            require(got == expected, f"{'+'.join(modifiers + [key])}: "
                    f"expected {expected!r}, got {got!r}")

        # A dead key and its compose sequence: us:intl's apostrophe is
        # dead_acute.
        self.control("layout us:intl")
        self.tap("apostrophe")
        self.tap("e")
        got = self.read_keys()
        results.append(["dead_acute e", [], got])
        require(got == "é", f"dead acute then e: expected 'é', got {got!r}")

        # German layout: the key at the US Y position types z.
        self.control("layout de")
        self.tap("y")
        got = self.read_keys()
        results.append(["de y", [], got])
        require(got == "z", f"de layout: expected 'z', got {got!r}")
        self.control("layout us")

        self.control("focus none")
        require(self.focus_state() == "nil", "keyboard leave did not unfocus")
        self.control(f"focus {surface}")
        require(self.focus_state() == "t", "keyboard enter did not refocus")
        self.tap("g")
        require(self.read_keys() == "g", "keys after refocus were lost")
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def drain(self):
        self.send("drain")
        return self.phase("drained").split()

    def key_repeat(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        a = KEY["a"]

        # 50 a second after 100 ms: a 450 ms hold gives about 18 events.
        self.control("repeat-info 50 100")
        self.control(f"key {a} press")
        time.sleep(0.45)
        self.control(f"key {a} release")
        held = self.drain()
        self.data["held"] = len(held)
        require(set(held) == {"a"} and 8 <= len(held) <= 30,
                f"a 450 ms hold gave {len(held)} events: {held}")
        require(self.drain() == [], "repeat continued after release")

        # A zero rate disables repeat.
        self.control("repeat-info 0 100")
        self.control(f"key {a} press")
        time.sleep(0.4)
        self.control(f"key {a} release")
        require(self.drain() == ["a"], "a zero rate still repeated")

        # Focus loss stops repeat.
        self.control("repeat-info 50 100")
        self.control(f"key {a} press")
        time.sleep(0.25)
        self.control("focus none")
        time.sleep(0.3)
        self.control(f"focus {surface}")
        unfocused = self.drain()
        self.data["until-focus-loss"] = len(unfocused)
        require(1 <= len(unfocused) <= 15,
                f"repeat ran on after focus loss: {len(unfocused)} events")
        self.control(f"key {a} release")
        require(self.drain() == [], "repeat resumed after refocus")
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def seat_changes(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        self.control(f"pointer enter {surface} 60.5 40.5")
        require(self.focus_state() == "t", "initial keyboard focus missing")
        self.control("repeat-info 30 100")
        self.control(f"key {KEY['a']} press")
        require(self.read_keys() == "a", "the held key was not delivered")
        self.control("capabilities 0")
        require(self.focus_state() == "nil", "losing the keyboard kept focus")
        interfaces = ("wl_pointer", "wl_keyboard", "wp_cursor_shape_device_v1")

        def devices():
            resources = self.status()["resources"]
            return [resources.get(interface, 0) for interface in interfaces]

        self.until(lambda: devices() == [0, 0, 0], "released seat devices", process)
        # `display-mouse-p' follows the seat, unlike on X and PGTK.
        require(self.mouse_state() == "nil", "display-mouse-p without a pointer")
        self.drain()
        require(self.drain() == [], "repeat continued after keyboard removal")
        self.control(f"key {KEY['a']} release")

        self.control("layout de")
        self.control("capabilities 3")
        self.until(lambda: devices() == [1, 1, 1],
                   "replacement keyboard and pointer", process)
        require(self.mouse_state() == "t", "display-mouse-p with the pointer back")
        self.control("focus none")
        self.control(f"focus {surface}")
        self.control(f"pointer enter {surface} 60.5 40.5")
        require(self.focus_state() == "t", "replacement keyboard did not focus")
        self.tap("y")
        require(self.read_keys() == "z", "replacement keyboard ignored its layout")
        self.control(f"key {KEY['y']} press")
        require(self.read_keys() == "z", "the held key ignored the new layout")
        self.control("layout us")
        self.sync_mock(process)
        self.drain()
        require(self.drain() == [], "repeat survived a keymap replacement")
        self.control(f"key {KEY['y']} release")
        self.tap("y")
        require(self.read_keys() == "y", "the changed keymap was not used")
        self.click()
        require([event[0] for event in self.clicks(2)] ==
                ["down-mouse-1", "mouse-1"], "replacement pointer lost button events")
        self.send("done")

    def input_queue(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        self.tap("a")
        self.send("queue-selection")
        self.phase("queue-selection")
        self.until(lambda: self.status()["selections_set"] == 1,
                   "published selection", process)
        self.send("queue-state")
        before_raw, before_dropped = self.phase("queue-state")
        # The FIFO overflows only if one dispatch brings more key
        # presses than it holds.  Emacs empties it after each dispatch,
        # so reading the burst as it arrives may never fill it.  Hold
        # Emacs until the burst and the request are in its socket.
        self.send("hold")
        self.phase("hold")
        self.control(f"key-burst {KEY['a']} 1024")
        self.control("request-selection text/plain;charset=utf-8")
        # Mockcomp sends a command's events after its reply, and before
        # it serves another command.
        self.status()
        (self.runtime / "resume.hold").touch()
        self.until(lambda: self.control("selection-result queue clipboard") == "ok",
                   "clipboard while input is queued", process)
        self.send("queue-state")
        raw, dropped = self.phase("queue-state")
        require(raw - before_raw == 2048 and dropped > before_dropped,
                f"input queue did not overflow: {raw - before_raw} raw keys,"
                f" {dropped - before_dropped} drops")
        self.drain()
        self.tap("g")
        require(self.read_keys() == "g", "keyboard did not recover from overflow")
        self.send("done")

    def clicks(self, count):
        self.send(f"clicks {count}")
        return [list(click) for click in self.phase("clicks")]

    def click(self, button=BTN_LEFT):
        self.control(f"pointer button {button} press")
        self.control(f"pointer button {button} release")

    def pointer(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        self.control(f"pointer enter {surface} 40.5 30.5")
        results = self.data["pointer-clicks"] = {}

        # Event times come from mockcomp's manual clock, so groups of
        # clicks are far apart and clicks within a group are close.
        self.control("clock 10000")
        self.click()
        got = results["click"] = self.clicks(2)
        require([c[0] for c in got] == ["down-mouse-1", "mouse-1"],
                f"a left click gave {got}")
        require(got[0][1:3] == [40, 30], f"click at 40.5, 30.5 landed at {got[0][1:3]}")

        self.control("clock 20000")
        self.click()
        self.click()
        got = results["double"] = self.clicks(4)
        require([c[0] for c in got] == ["down-mouse-1", "mouse-1",
                                       "double-down-mouse-1", "double-mouse-1"],
                f"a double click gave {got}")

        self.control(f"key {KEY['shift']} press")
        self.control("clock 30000")
        self.click()
        self.control(f"key {KEY['shift']} release")
        got = results["shift"] = self.clicks(2)
        require([c[0] for c in got] == ["S-down-mouse-1", "S-mouse-1"],
                f"a shifted click gave {got}")

        self.control("clock 40000")
        self.click(BTN_RIGHT)
        self.click(BTN_BACK)
        got = results["buttons"] = self.clicks(4)
        require([c[0] for c in got] == ["down-mouse-3", "mouse-3",
                                       "down-mouse-8", "mouse-8"],
                f"right and back buttons gave {got}")

        self.control("clock 50000")
        self.control("pointer button 272 press")
        self.control("pointer motion 120.5 60.5")
        # Dragging window edges measures the pointer by its absolute
        # position, which follows the drag outside the frame too.
        self.send("absolute")
        got = results["absolute"] = self.phase("absolute")
        require(got == [120, 60], f"mouse-absolute-pixel-position gave {got}")
        self.control("pointer motion 2000.5 -30.5")
        self.send("absolute")
        got = results["absolute-outside"] = self.phase("absolute")
        require(got == [2000, -31],
                f"outside the frame, mouse-absolute-pixel-position gave {got}")
        self.control("pointer motion 120.5 60.5")
        self.control("pointer button 272 release")
        got = results["drag"] = self.clicks(2)
        require(got[1][0] == "drag-mouse-1" and got[1][3:5] == [120, 60],
                f"a drag gave {got}")

        # Text under the pointer gets the text cursor.
        self.control("pointer motion 60.5 40.5")
        self.until(lambda: self.status()["cursor_shape"] == CURSOR_TEXT,
                   "the text cursor over text", process)

        # The mode line's buffer name has a mouse face: the hand cursor.
        self.send("geometry")
        mode_line_y, name_x, *_ = self.phase("geometry")
        self.control(f"pointer motion {name_x + 0.5} {mode_line_y + 0.5}")
        self.until(lambda: self.status()["cursor_shape"] == CURSOR_POINTER,
                   "the hand cursor over the mode line's buffer name", process)

        # At scale 2, logical coordinates double.
        self.control(f"preferred-scale {surface} 240 2")
        self.quiesce(process)
        self.control("pointer motion 40.5 30.5")
        self.control("clock 60000")
        self.click()
        got = results["scale-2"] = self.clicks(2)
        require(got[0][1:3] == [81, 61],
                f"at scale 2, a click at 40.5, 30.5 landed at {got[0][1:3]}")
        self.control(f"preferred-scale {surface} 120 1")
        self.quiesce(process)

        # A tool bar button gives a tool bar event, not a click.
        self.send("tool-bar")
        height, = self.phase("tool-bar")
        self.quiesce(process)
        before_hover = self.surface(surface)["content_commits"]
        self.control(f"pointer motion 12.5 {height // 2 + 0.5}")
        self.until(lambda: self.surface(surface)["content_commits"] > before_hover,
                   "tool bar hover redraw", process)
        self.control(f"pointer motion 80.5 {height + 20.5}")
        self.quiesce(process)
        # The motion onto the button and the press arrive together, so
        # Emacs dispatches the press before it handles the motion; the
        # release comes once it has.
        self.control("clock 70000")
        self.control(f"pointer press-at 12.5 {height // 2 + 0.5} {BTN_LEFT}")
        self.quiesce(process)
        self.control(f"pointer button {BTN_LEFT} release")
        got = results["tool-bar"] = self.clicks(2)
        require(not any(c[0].endswith("mouse-1") for c in got),
                f"a tool bar click gave plain clicks: {got}")
        require(any("tool-bar" in c[0] for c in got),
                f"a tool bar click gave no tool bar event: {got}")

        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def pointer_theme(self, process):
        """Without cursor-shape-v1, cursors come from the cursor theme
        as surfaces given to wl_pointer.set_cursor."""
        surface, _ = self.data["ready"]
        self.control(f"pointer enter {surface} 4.5 4.5")
        self.until(lambda: self.status()["cursor_sets"] >= 1,
                   "a themed cursor on enter", process)
        before = self.status()["cursor_sets"]
        self.control("pointer motion 60.5 40.5")
        self.until(lambda: self.status()["cursor_sets"] > before,
                   "the themed text cursor over text", process)
        # At scale 2 the theme is loaded at twice the size.
        self.control(f"preferred-scale {surface} 240 2")
        self.quiesce(process)
        before = self.status()["cursor_sets"]
        self.control(f"pointer enter {surface} 4.5 4.5")
        self.until(lambda: self.status()["cursor_sets"] > before,
                   "a themed cursor at scale 2", process)
        status = self.status()
        require(not status["cursor_hidden"] and status["cursor_shapes"] == 0,
                f"unexpected cursor state {status}")
        require(not status["violation_total"], "protocol violations")
        self.send("done")

    def wheel(self, count):
        self.send(f"wheel {count}")
        return [list(event) for event in self.phase("wheel")]

    def axis(self, *args):
        self.control("pointer axis " + " ".join(map(str, args)))

    def scroll(self, process):
        surface, _ = self.data["ready"]
        version = SEAT_VERSIONS.get(self.name, 8)
        self.control(f"focus {surface}")
        self.control(f"pointer enter {surface} 60.5 40.5")
        results = self.data["wheel-events"] = {}

        def types(events):
            return [event[0] for event in events]

        # Each event gets its own time, far beyond `double-click-time',
        # so Emacs counts no double wheel events.
        clock = iter(range(10000, 10000000, 10000))

        def axis(*args):
            self.control(f"clock {next(clock)}")
            self.axis(*args)

        def unit():
            """The pixels one X scroll unit moves the window."""
            self.send("geometry")
            return self.phase("geometry")[5] ** (2 / 3)

        def near(value, expected):
            return abs(value - expected) < 1e-6

        # One notch each way and sideways, each carrying one unit's pixel
        # delta, as on X.  Below version 5 there are no frames, discrete
        # steps or sources: each axis event is a notch.
        axis("vertical", 15, "wheel", 120)
        axis("vertical", -15, "wheel", -120)
        axis("horizontal", 15, "wheel", 120)
        got = results["notches"] = self.wheel(3)
        pixels = unit()
        require(types(got) == ["wheel-down", "wheel-up", "wheel-right"]
                and near(got[0][3], -pixels) and near(got[1][3], pixels)
                and near(got[2][2], -pixels),
                f"wheel notches gave {got}")
        # Wayland identifies no devices, but from version 5 an axis frame
        # says what scrolled.  Lisp asks for that through `device-class',
        # for one to tell a wheel's notches from a touchpad's travel.
        wheel_class = "mouse" if version >= 5 else "core-pointer"
        require({event[4] for event in got} == {wheel_class},
                f"wheel notches came from {[event[4] for event in got]}")

        self.control(f"key {KEY['shift']} press")
        axis("vertical", 15, "wheel", 120)
        self.control(f"key {KEY['shift']} release")
        got = results["shifted"] = self.wheel(1)
        require(types(got) == ["S-wheel-down"], f"a shifted notch gave {got}")

        if version >= 8:
            # High-resolution wheels: two half steps make one notch.
            axis("vertical", 7.5, "wheel", 60)
            axis("vertical", 7.5, "wheel", 60)
            got = results["half-steps"] = self.wheel(2)
            require(types(got) == ["wheel-down", "nil"],
                    f"two half steps gave {got}")

        if version >= 5:
            # Fingers move a scroll unit per 10 compositor pixels.
            self.send("coalesce nil")
            self.phase("coalesce")
            axis("vertical", 12.5, "finger")
            self.control(f"clock {next(clock)}")
            self.control("pointer stop vertical")
            got = results["finger"] = self.wheel(2)
            require(types(got) == ["wheel-down", "touch-end"]
                    and near(got[0][3], -1.25 * pixels),
                    f"a finger scroll gave {got}")
            require(got[0][4] == "touchpad", f"a finger scroll came from {got[0][4]}")
            # A continuous source, such as a trackpoint, scrolls the same way.
            axis("vertical", 12.5, "continuous")
            self.control(f"clock {next(clock)}")
            self.control("pointer stop vertical")
            got = results["continuous"] = self.wheel(2)
            require(types(got) == ["wheel-down", "touch-end"]
                    and near(got[0][3], -1.25 * pixels) and got[0][4] == "trackpoint",
                    f"a continuous scroll gave {got}")

            # Coalesced into whole units: half a unit gives nothing, and
            # the next half completes one, with no line count.
            self.send("coalesce t")
            self.phase("coalesce")
            axis("vertical", 5, "finger")
            axis("vertical", 5.5, "finger")
            got = results["coalesced"] = self.wheel(2)
            require(types(got) == ["wheel-down", "nil"] and got[0][1] == 0
                    and near(got[0][3], -1.05 * pixels),
                    f"a coalesced finger scroll gave {got}")
            self.control(f"clock {next(clock)}")
            self.control("pointer stop vertical")
            self.wheel(1)

            # Ten one-pixel steps make exactly one unit, each way.
            for sign, name in ((1, "wheel-down"), (-1, "wheel-up")):
                for _ in range(10):
                    axis("vertical", sign, "finger")
                got = results[f"exact-{name}"] = self.wheel(2)
                require(types(got) == [name, "nil"] and near(got[0][3], -sign * pixels),
                        f"ten one-pixel steps gave {got}")
                self.control(f"clock {next(clock)}")
                self.control("pointer stop vertical")
                self.wheel(1)

            # At scale 2 the same travel is the same unit, which the
            # window's doubled physical height enlarges.
            self.send("coalesce nil")
            self.phase("coalesce")
            self.control(f"preferred-scale {surface} 240 2")
            # The unit follows the window's height, so wait until Emacs
            # has applied the scale, not only for commits to pause.
            self.until(lambda: unit() > pixels * 1.5, "the window at scale 2", process)
            self.quiesce(process)
            self.control("pointer motion 60.5 40.5")
            axis("vertical", 10, "finger")
            got = results["finger-scale-2"] = self.wheel(1)
            scaled = unit()
            require(types(got) == ["wheel-down"] and near(got[0][3], -scaled)
                    and scaled > pixels,
                    f"a finger scroll at scale 2 gave {got}")
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def im(self):
        return self.status().get("im", {})

    def preedit(self):
        self.send("preedit-state")
        return self.phase("preedit")

    def typed(self):
        """Each frame's buffer text after applying the pending events."""
        self.send("type")
        first, second, others = self.phase("typed")
        self.data.setdefault("typed-others", []).append(others)
        return [first, second]

    def input_method(self, process):
        surface, _ = self.data["ready"]
        results = self.data["im-results"] = {}

        # Keyboard focus enters the text input, which Emacs enables.
        self.control(f"focus {surface}")
        self.until(lambda: self.im().get("enabled"), "an enabled input method",
                   process)
        im = results["enabled"] = self.im()
        require(im["entered"] and im["content"] == [0, 0],
                f"unexpected input method state {im}")

        # The cursor rectangle is point's glyph, in logical pixels (the
        # scale is 1 here).
        self.send("cursor-rect")
        rect = results["cursor"] = self.phase("cursor-rect")
        self.until(lambda: self.im()["rect"][:2] == rect[:2],
                   f"the cursor rectangle at {rect}", process)

        # Preedit text shows before point; a commit replaces it.
        self.control("im preedit 0 6 かな")
        self.control("im done")
        require(self.preedit() == "かな", "the preedit text is not shown")
        self.control("im commit 漢字")
        self.control("im done")
        got = results["commit"] = self.read_keys(2)
        require(got == "漢 字", f"the commit gave {got!r}")
        require(self.preedit() == "", "the commit left the preedit shown")

        # A rectangle held back by a mismatched done is dropped when
        # point returns to the rectangle already sent.
        self.control("im commit b")
        self.control("im done 999")
        require(self.read_keys() == "b", "a mismatched done lost its commit")
        held = self.im()
        for count in (3, -3):
            self.send(f"move {count}")
            self.phase("moved")
        self.control("im done")
        self.sync_mock(process)
        now = results["returned"] = self.im()
        require(now["rect"] == held["rect"],
                f"sent {now['rect']} after point returned to {held['rect']}")

        # After a done that does not count our commits, its edits still
        # apply, but the cursor rectangle waits for a matching done.
        self.control("im commit a")
        self.control("im done 999")
        require(self.read_keys() == "a", "a mismatched done lost its commit")
        held = self.im()
        self.send("move 3")
        self.phase("moved")
        self.sync_mock(process)
        now = self.im()
        require(now["rect"] == held["rect"] and now["commits"] == held["commits"],
                "the cursor rectangle was sent before a matching done")
        self.control("im done")
        self.until(lambda: self.im()["commits"] > held["commits"]
                   and self.im()["rect"] != held["rect"],
                   "the held cursor rectangle after a matching done", process)

        # A long multibyte commit between keys arrives whole and in
        # order, although it has more characters than the event queue.
        text = "かな漢字" * 75
        self.tap("a")
        self.control(f"im commit {text}")
        self.control("im done")
        self.tap("e")
        self.send("drain")
        got = self.phase("drained")
        require(got == " ".join(["a", *text, "e"]),
                f"a {len(text)}-character commit arrived as "
                f"{len(got.split())} events")
        results["long-commit"] = len(text)

        # A batch belongs to the frame focused when it arrived: if focus
        # moves before its done, even a done that does not count our
        # commits, its text goes to that frame and its preedit is
        # dropped.
        self.send("second-frame")
        second, _ = self.phase("second")
        self.until(lambda: self.status()["mapped"] == 2, "the second frame",
                   process)
        first_frame_rect = self.im()["rect"]
        self.control("im commit z")
        self.control("im preedit 0 3 か")
        self.control(f"focus {second}")
        self.control("im done 999")
        typed = results["in-flight"] = self.typed()
        require(typed == ["first: z", ""],
                f"a batch in flight went to {typed}")
        require(self.preedit() == "", "an unfocused frame's preedit was shown")
        self.control("im commit y")
        self.control("im done")
        typed = results["second-frame"] = self.typed()
        require(typed == ["first: z", "y"],
                f"the second frame's commit gave {typed}")
        # Entering the second frame does not resend the first frame's
        # rectangle: the first one sent is the second frame's point
        # before "y" was inserted, one character before it is now.
        self.send("cursor-rect")
        rect = self.phase("cursor-rect")
        first = results["first-rect"] = self.im()["first_rect"]
        require(first and first != first_frame_rect
                and first[:2] == [rect[0] - rect[2], rect[1]],
                f"the first rectangle after enter was {first}; the first "
                f"frame's was {first_frame_rect}, point is now at {rect}")

        # A done sent before the compositor saw the frame's deletion
        # arrives after it, with the display still live, and goes
        # nowhere.  Stalling the mock keeps the deletion unread.
        self.control("im commit x")
        self.sync_mock(process)
        self.control("stall on")
        self.send("delete-second")
        self.phase("deleted")
        self.control("im done")
        self.control("stall off")
        self.until(lambda: self.status()["mapped"] == 1,
                   "the second frame gone", process)
        self.control(f"focus {surface}")
        self.sync_mock(process)
        typed = results["after-delete"] = self.typed()[0]
        require(typed == "first: z",
                f"a deleted frame's commit went to {typed!r}")

        # With input methods off, the next focus does not enable one.
        self.send("use-im nil")
        self.phase("use-im")
        self.control("focus none")
        self.control(f"focus {surface}")
        self.sync_mock(process)
        require(not self.im()["enabled"], "the input method was enabled anyway")
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def quit_keys(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        for command in ("busy", "wait"):
            self.send(command)
            interrupt, = self.phase(command)
            time.sleep(0.3)
            self.tap("g", "ctrl")
            result, elapsed, hook_calls, raw_keys = self.phase("quit")
            self.data[command] = {"result": result, "seconds": elapsed,
                                  "interrupt-input": interrupt,
                                  "hook-calls": hook_calls, "raw-keys": raw_keys}
            require(result == "quit", f"C-g did not interrupt the {command} loop")
            require(elapsed < 2, f"C-g took {elapsed:.2f} s to interrupt {command}")
        self.send("done")

    def surface(self, identifier):
        return next((surface for surface in self.status()["toplevels"]
                     if surface["id"] == identifier), {})

    def frame_states(self, process):
        surface, _ = self.data["ready"]
        self.quiesce(process)

        def committed():
            return self.surface(surface)["content_commits"]

        self.control("frame-callbacks off")
        before = committed()
        self.m1_step("edit 0")
        self.until(lambda: committed() > before, "commit before the callback stalls",
                   process)
        held = committed()
        self.m1_step("edit 1")
        self.quiesce(process)
        require(committed() == held, "redisplay ignored the withheld callback")
        self.control("frame-callbacks on")
        self.until(lambda: committed() > held, "present after callback resumes", process)

        self.control(f"toplevel-state {surface} suspended")
        self.sync_mock(process)
        self.m1_step("resize")
        self.quiesce(process)
        require(self.surface(surface)["acked_latest"], "suspend configure was not acked")
        suspended = committed()
        self.m1_step("edit 2")
        self.quiesce(process)
        require(committed() == suspended, "suspended frame published a redraw")
        self.control(f"toplevel-state {surface} normal")
        self.sync_mock(process)
        self.m1_step("resize")
        self.until(lambda: self.surface(surface)["acked_latest"] and committed() > suspended,
                   "resume configure and redraw", process)

        self.resize("811 610 900 640 793 487", process)
        self.until(lambda: self.surface(surface)["acked_latest"]
                   and self.surface(surface)["buffer_size"] == [793, 487],
                   "last of overlapping configure sizes", process)

        def geometry():
            self.send("geometry")
            width, height, fullscreen = self.phase("geometry")
            self.phase("state")
            return [width, height], fullscreen

        def configure(state, size):
            self.control(f"toplevel-state {surface} {state} {size[0]} {size[1]}"
                         if size else f"toplevel-state {surface} {state}")
            self.sync_mock(process)
            self.m1_step("resize")
            self.until(lambda: self.surface(surface)["acked_latest"]
                       and (not size or self.surface(surface)["buffer_size"] == size),
                       f"{state} configure", process)

        # A size the compositor fixes survives implied and explicit
        # resizes, which take effect again once the frame floats.
        for state, size, parameter in (("fullscreen", [700, 500], "fullboth"),
                                       ("maximized", [660, 470], "maximized"),
                                       ("tiled", [640, 480], "fullheight")):
            configure(state, size)
            for step in ("font 200", "frame-size 400 300", "font 100"):
                self.m1_step(step)
                self.quiesce(process)
                shown, fullscreen = geometry()
                require(fullscreen == parameter,
                        f"{state}: the fullscreen parameter is {fullscreen}")
                require(shown == size and self.surface(surface)["buffer_size"] == size,
                        f"{state}: {step} resized the frame to {shown}, "
                        f"buffer {self.surface(surface)['buffer_size']}")
        configure("normal", None)
        self.m1_step("frame-size 400 300")
        self.quiesce(process)
        shown, fullscreen = geometry()
        require(fullscreen == "none", f"a floating frame is {fullscreen}")
        require(shown == self.surface(surface)["buffer_size"] and shown != [640, 480],
                f"a floating frame kept {shown} after a resize request")

        trace = self.artifacts / "primary.trace"
        for command, request in (("title", 'set_title("Wayland flush test")'),
                                 ("iconify", "set_minimized()")):
            self.send(command)
            self.phase(command)
            self.until(lambda: request in trace.read_text(),
                       f"{request} before redisplay", process)
            (self.runtime / f"resume.{command}").touch()
            self.phase("state")
        self.send("done")

    def minimal_protocols(self, process):
        surface, _ = self.data["ready"]
        self.control(f"focus {surface}")
        self.tap("a")
        self.send("key")
        self.phase("key")
        self.send("done")

    def scale_frames(self, process):
        first, _ = self.data["ready"]
        second, _ = self.phase("second")
        self.until(lambda: self.status()["mapped"] == 2, "both frames mapped", process)
        def redraw():
            self.send("redraw")
            frame, other, display, monitor_changes = self.phase("state")
            self.data["display"] = display
            self.data["monitor-changes"] = monitor_changes
            return frame, other

        redraw()
        before_add = self.data["monitor-changes"]
        self.control("output add")
        self.sync_mock(process)

        for surface in (first, second):
            self.control(f"move-output {surface} 1")
            self.control(f"configure-surface {surface} 801 481")
        self.sync_mock(process)
        redraw()
        self.until(lambda: all(self.surface(surface).get("acked_latest")
                               for surface in (first, second)),
                   "both initial sizes configured", process)
        initial, unaffected = redraw()

        self.control(f"move-output {first} 2")
        if self.name == "scale-fractional":
            scales = (150, 180, 240)
        else:
            scales = (240,)
        self.data["scale-changes"] = []
        for scale in scales:
            sequence = self.surface(first)["content_commits"] + 1
            if self.name == "scale-fractional":
                self.control(f"configure-surface {first} 817 489 801 481")
            self.control(f"preferred-scale {first} {scale} 2")
            self.sync_mock(process)
            width, height = ((801 * scale + 60) // 120, (481 * scale + 60) // 120)

            def settled():
                frame, other = redraw()
                status = self.surface(first)
                if status.get("buffer_size") != [width, height] or not status["acked_latest"]:
                    return None
                require(frame[:2] == [width, height], "frame and buffer size disagree")
                require(all(a > b for a, b in zip(frame[2:5], initial[2:5])),
                        "fonts and glyph geometry did not scale with the frame")
                require(other == unaffected, "other frame changed during scale transition")
                require(frame[-1] == "MOCK-2", "frame monitor did not change")
                # MOCK-2 is 150%, which wl_output.scale reports as 2; only
                # xdg-output gives its logical size.
                require(frame[-2] == [1920, 0, 1280, 720],
                        f"MOCK-2 geometry {frame[-2]} is not its logical size")
                require(self.data["display"] == [3200, 1080],
                        f"display size {self.data['display']} is not logical")
                require(self.data["monitor-changes"] > before_add,
                        "adding an output did not notify the monitor hook")
                return frame, status

            frame, status = self.until(settled, f"scale {scale}/120", process)
            self.snapshot(sequence, process, first)
            if self.name == "scale-fractional":
                require(status["scale"] == 1 and status["viewport"] == [801, 481],
                        "fractional buffer was not mapped to its logical size")
            else:
                require(status["scale"] == 2 and status["viewport"] == [0, 0],
                        "whole-number fallback did not set buffer scale")
            self.data["scale-changes"].append([scale, frame, status])

        before_remove = self.data["monitor-changes"]
        self.control("output remove")
        if self.name != "scale-outputs":
            self.control(f"preferred-scale {first} 120 1")
        self.sync_mock(process)

        def restored():
            frame, other = redraw()
            if (frame[:5] != initial[:5] or frame[-1] != "MOCK-1"
                    or frame[-2] != [0, 0, 1920, 1080]
                    or self.data["display"] != [1920, 1080]
                    or self.data["monitor-changes"] <= before_remove):
                return False
            require(other == unaffected, "second frame changed on output removal")
            return self.surface(first).get("buffer_size") == initial[:2]

        self.until(restored, "scale fallback after output removal", process)
        self.send("done")

    def check_child_map_position(self, parent, child, position, start=0):
        trace = self.emacs_log()
        role = re.search(rf"get_subsurface\(new id wl_subsurface#(\d+), "
                         rf"wl_surface#{child}, wl_surface#{parent}\)", trace)
        require(role is not None, "missing subsurface role")
        x, y = position
        offset = trace.find(f"wl_subsurface#{role[1]}.set_position({x}, {y})",
                            start)
        commit = trace.find(f"wl_surface#{parent}.commit()", max(start, offset))
        attach = trace.find(f"wl_surface#{child}.attach(wl_buffer#", start)
        require(start <= offset < commit < attach,
                f"child {child} mapped before publishing position {position}")

    def child_frames(self, process):
        root, _ = self.data["ready"]
        before = self.surface(root)
        self.send("create")
        child, _ = self.phase("created")

        def child_state():
            return next((item for item in self.status()["subsurfaces"]
                         if item["id"] == child), {})

        def check_shown():
            state = child_state()
            return state if state.get("mapped") else None

        state = self.until(check_shown, "a child on an idle parent", process)
        require(state["parent"] == root and state["position"] == [17, 23]
                and not state["synchronized"], f"wrong child association: {state}")
        self.check_child_map_position(root, child, (17, 23))
        # Child setup can still leave a parent repaint pending.
        self.quiesce(process, interval=0.15 * self.args.timeout_scale)
        state = child_state()
        parent = self.surface(root)
        require(parent["commits"] > before["commits"],
                "the parent did not commit the association")
        self.send("redraw")
        self.phase("redrawn")
        self.until(lambda: child_state().get("content_commits", 0)
                   > state["content_commits"], "independent child redraw", process)
        require(self.surface(root)["commits"] == parent["commits"],
                "child redraw committed the parent")
        self.send("block-parent")
        self.phase("blocked")
        _, _, width, height = parent["geometry"]
        self.control(f"configure-surface {root} {width + 32} {height + 32}")
        self.sync_mock(process)
        self.send("move")
        self.phase("moved")
        self.until(lambda: child_state().get("position") == [-39, -29],
                   "the final child position", process)
        moved = self.surface(root)
        require(moved["geometry"] == parent["geometry"]
                and parent["geometry"][2] > 0 and parent["geometry"][3] > 0,
                "overflow changed root geometry")
        require(moved["commits"] == parent["commits"] + 1
                and moved["content_commits"] == parent["content_commits"]
                and not moved["acked_latest"], "parent state committed an early resize")
        self.send("unblock-parent")
        self.phase("unblocked")
        self.until(lambda: self.surface(root)["acked_latest"],
                   "parent resize after allocation resumes", process)
        for command, phase, mapped in (("hide", "hidden", False),
                                       ("show", "shown", True)):
            start = len(self.emacs_log())
            self.send(command)
            self.phase(phase)
            self.until(lambda: child_state().get("mapped") is mapped,
                       f"child {phase}", process)
            if mapped:
                self.check_child_map_position(root, child, (31, 41), start)
        self.send("delete")
        self.phase("deleted")
        self.until(lambda: not child_state(), "child deleted with parent", process)
        self.send("done")

    def child_state(self, identifier):
        return next((item for item in self.status()["subsurfaces"]
                     if item["id"] == identifier), {})

    def child_hierarchy(self, process):
        root, _ = self.data["ready"]
        self.control(f"focus {root}")
        self.send("create")
        child, grand, sibling = self.phase("created")

        def attached(identifier, parent):
            state = self.child_state(identifier)
            return state.get("mapped") and state.get("parent") == parent

        def check_stacking(bottom, top):
            low = self.child_state(bottom).get("stacking", 0)
            high = self.child_state(top).get("stacking", 0)
            return 0 < low < high

        self.until(lambda: attached(grand, child) and attached(sibling, child),
                   "the nested children to map", process)
        require(self.child_state(child)["stacking"] > 0,
                "lowering an only child put it behind its parent")
        require(check_stacking(sibling, grand), "restack against parent failed")
        for operation, bottom, top in (("raise-sibling", grand, sibling),
                                       ("lower-sibling", sibling, grand),
                                       ("above-sibling", grand, sibling),
                                       ("below-sibling", sibling, grand)):
            self.send(operation)
            self.phase(operation)
            self.until(lambda: check_stacking(bottom, top), operation, process)
        self.send("focus")
        x, y, grand_width, child_width = self.phase("focused")
        self.until(lambda: self.im()["rect"][:2] == [x + 28, y + 34],
                   f"nested cursor rectangle at {[x + 28, y + 34]}", process)
        self.control("im commit z")
        self.control("im done")
        self.send("input")
        self.phase("typed")
        outside = grand_width - 2.5
        require(outside + 11 >= child_width, "grandchild does not extend past its parent")
        self.control(f"pointer enter {grand} {outside} 21.5")
        self.click()
        self.send("pointer")
        self.phase("clicked")
        self.send("hide")
        self.phase("hidden")
        self.until(lambda: not self.child_state(grand).get("mapped"),
                   "descendant hidden with its ancestor", process)
        for request, phase in (("promote", "promoted"), ("demote", "demoted")):
            self.send(request)
            replacement = self.phase(phase)
            require(replacement != child, "surface reused across roles")
            child = replacement
            self.until(lambda: attached(grand, child) and attached(sibling, child),
                       f"surviving children after {request}", process)
            require(check_stacking(sibling, grand),
                    "role change lost the sibling stack")
        self.send("reparent")
        require(self.phase("reparented") == grand, "reparent replaced a child surface")
        self.until(lambda: attached(grand, root), "child reparented", process)
        self.send("minibuffer")
        old, mini = self.phase("minibuffer")
        require(old != mini, "minibuffer promotion kept the child role")
        self.until(lambda: self.surface(mini).get("mapped"),
                   "promoted minibuffer frame to map", process)
        self.send("done")

    def child_pointer(self, process):
        """Highlight and blanking follow the pointer's frame, and neither
        outlives a child frame deleted under the pointer."""
        root, _ = self.data["ready"]
        self.control(f"focus {root}")
        self.send("create")
        child = self.phase("created")
        self.until(lambda: self.child_state(child).get("mapped"),
                   "the child to map", process)
        # Past the left fringe, on the first characters.
        self.control(f"pointer enter {child} 20.5 6.5")
        self.control("pointer motion 24.5 6.5")
        self.until(lambda: self.status()["cursor_shape"] == CURSOR_POINTER,
                   "the hand cursor over the child's highlighted text", process)
        # Over the child at 20, 20, the absolute position is in the
        # parent's coordinates.
        self.send("absolute")
        got = self.phase("absolute")
        require(got == [44, 26], f"over the child, mouse-absolute-pixel-position gave {got}")
        self.send("blank")
        self.phase("blanked")
        self.until(lambda: self.status()["cursor_hidden"],
                   "the pointer blanked over the child", process)
        self.send("delete")
        self.phase("deleted")
        # The compositor gives the pointer to the parent.  It shows there,
        # since only the deleted child blanked it.
        shapes = self.status()["cursor_shapes"]
        self.control(f"pointer enter {root} 60.5 40.5")
        self.until(lambda: not self.status()["cursor_hidden"]
                   and self.status()["cursor_shapes"] > shapes,
                   "a visible cursor over the parent", process)
        for x in (62.5, 63.5, 64.5):
            self.control(f"pointer motion {x} 40.5")
        self.quiesce(process)
        # Blanking over the parent, and moving the pointer shows it again.
        self.send("blank-root")
        self.phase("blanked")
        self.until(lambda: self.status()["cursor_hidden"],
                   "the pointer blanked over the parent", process)
        self.control("pointer motion 70.5 40.5")
        self.until(lambda: not self.status()["cursor_hidden"],
                   "the pointer shown again after motion", process)
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def child_drag(self, process):
        """Dragging window edges and frames measures the pointer with
        `mouse-absolute-pixel-position'.  While a button is held,
        compositors report the pointer relative to where the pressed
        frame was at the press, also after Lisp moves the frame."""
        root, _ = self.data["ready"]
        self.control(f"focus {root}")
        self.send("create")
        child = self.phase("created")
        self.until(lambda: self.child_state(child).get("mapped"),
                   "the child to map", process)
        self.control(f"pointer enter {child} 20.5 6.5")
        self.send("absolute")
        got = self.phase("absolute")
        require(got == [40, 26], f"over the child at 20, 20, the position was {got}")
        self.control(f"pointer button {BTN_LEFT} press")
        self.send("move")
        self.phase("moved")
        self.until(lambda: self.child_state(child).get("position") == [60, 40],
                   "the child at its new position", process)
        self.control("pointer motion 30.5 6.5")
        self.send("absolute")
        got = self.phase("absolute")
        require(got == [50, 26],
                f"after the child moved under the held button, the position was {got}")
        self.control(f"pointer button {BTN_LEFT} release")
        require(not self.status()["violation_total"], "protocol violations")
        self.send("done")

    def child_scale(self, process):
        root, _ = self.data["ready"]
        self.control(f"preferred-scale {root} 180 2")
        self.send("scale")
        self.phase("scaled")
        self.until(lambda: self.surface(root)["scale"] == 2,
                   "root at integer scale 2", process)
        self.send("create")
        child = self.phase("created")
        state = self.until(lambda: self.child_state(child)
                           if self.child_state(child).get("mapped") else None,
                           "child at integer scale 2", process)
        self.send("measure")
        width, height, _ = self.phase("measured")
        require(state["scale"] == 2 and state["viewport"] == [0, 0]
                and state["buffer_size"] == [width + width % 2, height + height % 2]
                and state["position"] == [20, 15], f"wrong integer child {state}")
        require(width % 2 or height % 2, "test frame has no padding")
        x, y = (width / 2, 2) if width % 2 else (2, height / 2)
        self.control(f"pointer enter {child} {x} {y}")
        self.click()
        self.send("padding")
        self.phase("padding")
        self.send("done")

    def child_mixed_scale(self, process):
        root, _ = self.data["ready"]
        self.send("create")
        child, grand, other = self.phase("created")
        self.control("output add")
        self.sync_mock(process)
        self.control(f"move-output {root} 2")
        for scale in (150, 180, 240):
            before = self.child_state(child).get("content_commits", 0)
            self.control(f"preferred-scale {root} {scale} 2")
            self.send("scale")
            size = self.phase("scaled")
            state = self.until(lambda: self.child_state(child) if
                               self.child_state(child).get("content_commits", 0)
                               > before else None, "scaled child redraw", process)
            require(state["buffer_size"][0] >= size[0]
                    and state["buffer_size"][1] >= size[1],
                    f"child buffer smaller than its content: {state}")
            require(state["position"] == [40, 30]
                    and self.child_state(grand)["position"] == [16, 12],
                    "scaling moved a child in logical coordinates")
            require(self.surface(other)["scale"] == 1,
                    "independent root inherited another root's scale")
        self.send("done")

    def selection(self, process):
        root, _ = self.data["ready"]
        self.send("local-primary")
        self.phase("local-primary")
        self.send("open-child")
        child = self.phase("child")
        self.until(lambda: self.child_state(child).get("mapped"),
                   "clipboard child mapped", process)
        self.control(f"focus {root}")
        self.tap("a")
        self.send("publish-a")
        self.phase("published-a")
        self.until(lambda: self.status()["selections_set"] == 1,
                   "the first Wayland selection", process)
        self.send("close-child")
        self.phase("child-closed")
        self.until(lambda: not self.child_state(child), "clipboard child deleted", process)
        self.control("request-selection text/plain;charset=utf-8")
        self.send("publish-b")
        self.phase("published-b")
        self.until(lambda: self.control("selection-result first") == "ok",
                   "the old source's in-flight transfer", process)
        self.until(lambda: self.status()["selections_set"] == 2,
                   "the replacement Wayland selection", process)
        self.control("focus none")
        self.sync_mock(process)
        self.send("unfocused")
        self.phase("unfocused")
        require(self.status()["selections_set"] == 2,
                "unfocused copy replaced the system clipboard")
        self.control(f"focus {root}")
        self.control("offer-selection external 日本語")
        self.sync_mock(process)
        self.send("foreign")
        self.phase("foreign")
        self.control("focus none")
        self.sync_mock(process)
        self.send("unavailable")
        self.phase("unavailable")
        self.control(f"focus {root}")
        self.sync_mock(process)
        self.control("hold-offer-pipe")
        self.send("timeout")
        self.phase("timed-out")
        require(self.control("offer-pipe-result") == "ok",
                "a timed-out clipboard read left its pipe open")
        self.send("read-quit")
        self.until(lambda: self.status()["held_offer_pipe"],
                   "clipboard read waiting for a reply", process)
        self.tap("g", "ctrl")
        self.phase("read-quit")
        require(self.control("offer-pipe-result") == "ok",
                "C-g left the clipboard pipe open")
        self.send("publish-slow")
        size, timeout_ms = self.phase("published-slow")
        self.until(lambda: self.status()["selections_set"] == 3,
                   "the slow clipboard selection", process)
        self.control("request-selection text/plain;charset=utf-8")
        start = time.monotonic()
        total = 0

        def read_chunk():
            result = self.control("selection-chunk a")
            return result if result != "pending" else None

        while (chunk := self.until(read_chunk, "the next clipboard chunk",
                                   process)) != "eof":
            total += int(chunk)
            time.sleep(0.012)
        elapsed_ms = (time.monotonic() - start) * 1000
        self.data["slow-selection"] = {"bytes": total, "elapsed_ms": elapsed_ms}
        require(total == size and elapsed_ms > timeout_ms,
                f"slow clipboard read stopped at {total} bytes")
        self.send("slow-done")
        self.send("done")

    def activation(self, process):
        root, socket = self.data["ready"]
        self.send("without-serial")
        self.phase("without-serial")
        status = self.until(lambda: self.status()["activation"]
                            if self.status()["activation"][0] == 1 else None,
                            "activation without an input serial", process)
        require(status[:4] == [1, 1, root, 0], f"unexpected serial-free activation {status}")
        self.control(f"focus {root}")
        self.tap("a")
        self.sync_mock(process)
        self.execute([self.args.emacsclient, "--socket-name", socket, "--no-wait",
                      self.runtime / "m3-activation-buffer"],
                     dict(self.env, XDG_ACTIVATION_TOKEN="client-test-token"))
        status = self.until(lambda: self.status()["activation"]
                            if self.status()["activation"][0] == 2 else None,
                            "emacsclient's activation token", process)
        require(status[:3] == [2, 1, root] and status[-1] == "client-test-token",
                f"unexpected client activation {status}")
        self.send("request")
        target = self.phase("requested")
        status = self.until(lambda: self.status()["activation"]
                            if self.status()["activation"][0] == 3 else None,
                            "a requested activation token", process)
        require(status[:3] == [3, 2, target] and status[3] and
                status[-1] == "mock-requested-token", f"wrong activation {status}")
        for count, operation in enumerate(("title", "replace-role"), 4):
            self.send(operation)
            target = self.phase(operation)
            status = self.until(lambda: self.status()["activation"]
                                if self.status()["activation"][0] >= count else None,
                                f"activation after {operation}", process)
            require(status[:3] == [count, 2, target] and status[-1] == operation,
                    f"wrong pending activation after {operation}: {status}")
        self.send("done")

    def parity_capture(self, display, edges, marker, path, process):
        left, top, right, bottom = edges
        geometry = f"{left},{top} {right - left}x{bottom - top}"
        env = dict(self.env, WAYLAND_DISPLAY=display)

        def capture():
            result = subprocess.run(
                [self.args.grim, "-t", "ppm", "-g", geometry, path],
                env=env, capture_output=True, timeout=self.remaining())
            require(result.returncode == 0,
                    f"grim failed: {result.stderr.decode(errors='replace')}")
            image = ppm_pixels(path.read_bytes())
            require(image[:2] == (right - left, bottom - top),
                    f"unexpected pixel dimensions for {path.name}")
            # Emacs can finish redisplay before sway displays the frame.
            return image if has_rgb(image[2], marker) else None

        return self.until(capture, f"the presented scene for {path.name}", process)

    def sway_clipboard(self):
        copy = shutil.which("wl-copy")
        paste = shutil.which("wl-paste")
        require(copy and paste, "wl-copy and wl-paste are required")
        display, ipc = self.sway()
        env = dict(self.env, WAYLAND_DISPLAY=display)
        keyboard = subprocess.Popen([HERE / "observer" / "vkbd"], env=env,
                                    stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                    text=True)
        self.stack.callback(self.stop, keyboard)
        require(keyboard.stdout.readline().strip() == "ready", "vkbd failed")
        process = self.gui(display)
        self.sway_mapped(ipc, process)
        mime = "text/plain;charset=utf-8"

        def type_a():
            keyboard.stdin.write(f"{KEY['a']}\n")
            keyboard.stdin.flush()
            require(keyboard.stdout.readline().strip() == "ok", "vkbd failed")

        def get_selection(primary=False):
            args = [paste, "--no-newline", "--type", mime]
            if primary:
                args.append("--primary")
            reader = subprocess.Popen(args, env=env, stdout=subprocess.PIPE,
                                      stderr=subprocess.PIPE)
            self.stack.callback(self.stop, reader)
            return reader

        def read_selection(reader):
            payload, stderr = reader.communicate(timeout=self.remaining())
            require(reader.returncode == 0, f"wl-paste failed: {stderr!r}")
            return payload

        def set_selection(payload, primary=False):
            args = [copy, "--foreground", "--type", mime]
            if primary:
                args.append("--primary")
            with tempfile.TemporaryFile(dir=self.runtime) as source:
                source.write(payload)
                source.seek(0)
                setter = subprocess.Popen(args, env=env, stdin=source)
            self.stack.callback(self.stop, setter)

        sizes = (1024, 1048576, 67108864)
        for size in sizes:
            self.send("publish")
            type_a()
            self.phase("published")
            self.until(lambda: mime in self.execute([paste, "--list-types"], env),
                       "the Emacs clipboard MIME type", process)
            reader = get_selection()
            if size == sizes[-1]:
                # Keep one read behind a full pipe while aborting another.
                aborted = get_selection()
                time.sleep(0.15)
                require(reader.poll() is None, "large write did not wait for reader")
                self.stop(aborted)
            payload = read_selection(reader)
            require(len(payload) == size and payload.count(b"a") == size,
                    f"wrong payload from Emacs at {size} bytes")
            expected = hashlib.sha256(payload).hexdigest()
            set_selection(payload)
            self.send("read")
            require(self.phase("read") == expected, f"wrong incoming {size}-byte hash")
        self.send("primary")
        type_a()
        self.phase("primary")
        self.until(lambda: mime in self.execute([paste, "--primary", "--list-types"], env),
                   "the Emacs PRIMARY MIME type", process)
        require(read_selection(get_selection(primary=True)) == b"primary from Emacs",
                "wrong outgoing PRIMARY")
        set_selection(b"primary from wl-copy", primary=True)
        self.send("read-primary")
        self.phase("read-primary")
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")

    def sway_child(self):
        from PIL import Image, ImageChops

        scale, mode = CHILD_SWAY_SCALES[self.name]
        display, ipc = self.sway('for_window [app_id=".+"] fullscreen enable\n',
                                 mode, scale)
        env = self.scene_environment()
        env.update(EMACS_WAYLAND_TEST="child-parity",
                   EMACS_WAYLAND_TEST_SIZE=mode.replace("x", " "))
        reference = {}
        builds = [("wayland", self.args.emacs)]
        if self.name == CHILD_PARITY_CASE:
            builds.insert(0, ("pgtk", self.args.pgtk_emacs))
        for backend, emacs in builds:
            log = f"{backend}.log"
            process = self.gui(display, emacs, log, env)
            require(self.data["ready"] == backend, f"wrong backend for {log}")
            for command, phase, origin in (("create", "created", (120, 90)),
                                           ("move", "moved", (240, 180)),
                                           ("hide", "hidden", None)):
                self.send(command)
                size = self.phase(phase)
                expected = None if origin is None else (
                    *origin, origin[0] + size[0], origin[1] + size[1])
                path = self.artifacts / f"{backend}-{phase}.png"

                def capture():
                    self.execute([self.args.grim, "-o", "HEADLESS-1", path],
                                 dict(self.env, WAYLAND_DISPLAY=display))
                    with Image.open(path) as source:
                        image = source.convert("RGB")
                    marker = Image.new("RGB", image.size, (212, 118, 42))
                    red, green, blue = ImageChops.difference(image, marker).split()
                    diff = ImageChops.lighter(ImageChops.lighter(red, green), blue)
                    box = diff.point([255] + [0] * 255).getbbox()
                    self.data[f"{backend}-{phase}-bounds"] = box
                    return image if box == expected else None

                image = self.until(capture, f"{backend} {phase} at {expected}",
                                   process)
                # Compare the popup and its surroundings. The root minibuffer
                # and the window-system furniture sit outside this rectangle.
                pixels = image.crop((40, 40, 550, 430)).tobytes()
                if backend == "pgtk":
                    reference[phase] = pixels
                elif reference:
                    require(pixels == reference[phase],
                            f"{phase} pixels differ from PGTK")
                tree = json.loads(self.execute([self.args.swaymsg, "-s", ipc,
                                                "-t", "get_tree"]))

                def windows(node):
                    return bool(node.get("app_id")) + sum(
                        windows(child) for child in node["nodes"] + node["floating_nodes"])

                require(windows(tree) == 1, "the popup created a second toplevel")
            self.send("done")
            self.finish(process)
            require(not self.protocol_errors(log), f"{backend} protocol error")

    def font_environment(self):
        """Use the pinned fonts with an explicit monospace alias."""
        fonts = self.args.fonts.resolve()
        rendering = ""
        if self.name in ("damage-repair-rgb", "subpixel-redraw"):
            rendering = (
                '<match target="font">'
                '<edit name="rgba" mode="assign"><const>rgb</const></edit>'
                '<edit name="antialias" mode="assign"><bool>true</bool></edit>'
                '<edit name="lcdfilter" mode="assign">'
                '<const>lcddefault</const></edit></match>')
        fontconfig = self.artifacts / "fonts.conf"
        fontconfig.write_text(
            "<?xml version=\"1.0\"?><!DOCTYPE fontconfig SYSTEM \"urn:fontconfig:fonts.dtd\">\n"
            f"<fontconfig><dir>{escape(str(fonts))}</dir>"
            f"<cachedir>{escape(str(self.runtime / 'font-cache'))}</cachedir>"
            "<alias><family>monospace</family>"
            "<prefer><family>DejaVu Sans Mono</family></prefer></alias>"
            f"{rendering}</fontconfig>\n", encoding="utf-8")
        return {"FONTCONFIG_FILE": str(fontconfig)}

    def scene_environment(self):
        config = self.runtime / "config"
        settings = config / "glib-2.0/settings/keyfile"
        settings.parent.mkdir(parents=True, exist_ok=True)
        # The packaged PGTK wrapper exposes GNOME's schema defaults.
        # Use Cairo's effective font options for both test backends.
        settings.write_text("[org/gnome/desktop/interface]\n"
                            "font-hinting='full'\n"
                            "font-antialiasing='grayscale'\n", encoding="utf-8")
        return {**self.font_environment(), "GDK_BACKEND": "wayland",
                "GDK_SCALE": "1", "GDK_DPI_SCALE": "1", "GTK_THEME": "Adwaita",
                "GSETTINGS_BACKEND": "keyfile", "XDG_CONFIG_HOME": str(config)}

    def pixel_parity(self, display):
        env = self.scene_environment()
        fonts = self.args.fonts.resolve()
        reference = {}
        differences = []
        for backend, emacs in (("pgtk", self.args.pgtk_emacs),
                               ("wayland", self.args.emacs)):
            log = f"{backend}.log"
            process = self.gui(display, emacs, log, env)
            ready = self.data.pop("ready")
            require(ready[0] == backend and ready[1]
                    and Path(ready[1]) == fonts / "DejaVuSansMono.ttf",
                    f"{backend} did not use the requested backend and font: {ready}")
            self.data[f"{backend}-scene"] = ready
            for phase in PARITY_PHASES:
                self.send(phase)
                edges, marker = self.phase(phase)
                path = self.artifacts / f"{backend}-{phase}.ppm"
                image = self.parity_capture(display, edges, marker, path, process)
                if has_rgb(image[2], (255, 0, 0)) != (phase in ("hbar", "hollow")):
                    differences.append(f"{backend} {phase}: wrong cursor visibility")
                if phase.startswith("canvas-"):
                    pixels = image[2]
                    rgb = zip(pixels[0::3], pixels[1::3], pixels[2::3])
                    color = tuple(marker)
                    if sum(pixel == color for pixel in rgb) != 96 * 96:
                        differences.append(f"{backend} {phase}: wrong canvas area")
                    if phase == "canvas-refresh" and has_rgb(
                            pixels, self.data["canvas-initial"][1]):
                        differences.append(f"{backend} {phase}: stale canvas pixels")
                else:
                    if not has_rgb(image[2], (0, 128, 0)):
                        differences.append(f"{backend} {phase}: missing fringe bitmap")
                    if not has_rgb(image[2], (0, 0, 255)):
                        differences.append(f"{backend} {phase}: missing inline image")
                if backend == "pgtk":
                    reference[phase] = image
                elif image != reference[phase]:
                    differences.append(f"{phase}: pixels differ from PGTK")
            self.send("done")
            self.finish(process)
            require(not self.protocol_errors(log), f"{backend} protocol error")
        require(not differences, "; ".join(differences))

    def image_parity(self, display):
        """Each image phase must match PGTK pixel for pixel, and each GIF
        phase show its own frame."""
        env = self.scene_environment()
        reference = {}
        differences = []
        for backend, emacs in (("pgtk", self.args.pgtk_emacs),
                               ("wayland", self.args.emacs)):
            log = f"{backend}.log"
            process = self.gui(display, emacs, log, env)
            ready = self.data.pop("ready")
            require(ready[0] == backend, f"{backend} used the wrong backend: {ready}")
            for index, phase in enumerate(IMAGE_PHASES):
                self.send(phase)
                edges, marker, frame = self.phase(phase)
                path = self.artifacts / f"{backend}-{phase}.ppm"
                image = self.parity_capture(display, edges, marker, path, process)
                self.check_image_phase(phase, index, frame, image[2])
                if backend == "pgtk":
                    reference[phase] = image
                elif image != reference[phase]:
                    differences.append(f"{phase}: pixels differ from PGTK")
            self.send("done")
            self.finish(process)
            require(not self.protocol_errors(log), f"{backend} protocol error")
        require(not differences, "; ".join(differences))

    @staticmethod
    def check_image_phase(phase, index, frame, pixels):
        if phase == "canvas-refresh":
            require(not has_rgb(pixels, IMAGE_CANVAS_COLOR),
                    "the canvas kept its old color")
            return
        require(frame == index, f"{phase}: the GIF is at frame {frame}")
        require(has_rgb(pixels, GIF_FRAME_COLORS[index]),
                f"{phase}: GIF frame {index} not shown")
        if index:
            require(not has_rgb(pixels, GIF_FRAME_COLORS[index - 1]),
                    f"{phase}: GIF frame {index - 1} still shown")
        require(has_rgb(pixels, IMAGE_CANVAS_COLOR), "the initial canvas is missing")

    def sway_scaled_images(self):
        """The image scene at a scaled output, against a reviewed
        reference.  The window's region is cut from a capture of the
        whole output, since grim takes regions in logical pixels."""
        from PIL import Image, ImageChops

        scale, mode = IMAGE_SCALES[self.name]
        display, ipc = self.sway('for_window [app_id=".+"] fullscreen enable\n',
                                 mode, scale)
        outputs = json.loads(self.execute([self.args.swaymsg, "-s", ipc, "-t", "get_outputs"]))
        output = next(output for output in outputs if output["name"] == "HEADLESS-1")
        logical = [output["rect"][axis] for axis in ("width", "height")]
        physical = tuple((value * scale + 50) // 100 for value in logical)
        env = self.scene_environment()
        env.update(EMACS_WAYLAND_TEST="image-parity",
                   EMACS_WAYLAND_TEST_SIZE=" ".join(map(str, physical)))
        process = self.gui(display, extra_env=env)
        self.data.pop("ready")
        capture_path = self.artifacts / "output.png"

        references = HERE / "references"
        reference = references / f"images-{scale}.png"
        require(reference.is_file(), f"missing reviewed reference {reference.name}")
        with Image.open(reference) as image:
            initial = image.convert("RGB")
        differences = []
        for index, phase in enumerate(IMAGE_PHASES):
            self.send(phase)
            edges, marker, frame = self.phase(phase)

            def capture():
                self.execute([self.args.grim, "-o", "HEADLESS-1", capture_path],
                             dict(self.env, WAYLAND_DISPLAY=display))
                with Image.open(capture_path) as image:
                    region = image.convert("RGB").crop(tuple(edges))
                return region if has_rgb(region.tobytes(), tuple(marker)) else None

            region = self.until(capture, f"the scaled image scene at {phase}", process)
            candidate = self.artifacts / f"{phase}.png" if index else self.artifacts / reference.name
            region.save(candidate)
            if index:
                # Store only the phase's pixel changes from the initial
                # scene.  Modular subtraction keeps the full RGB values
                # recoverable and the unchanged areas compress to zero.
                delta = references / f"images-{scale}-{phase}.png"
                require(delta.is_file(), f"missing reviewed reference {delta.name}")
                with Image.open(delta) as image:
                    expected = ImageChops.add_modulo(initial, image.convert("RGB"))
            else:
                expected = initial
            pixels = region.tobytes()
            try:
                self.check_image_phase(phase, index, frame, pixels)
                require(region.size == initial.size,
                        f"{phase} has the wrong capture size")
                require(pixels == expected.tobytes(),
                        f"{phase} differs from the reviewed pixels")
            except Failure as error:
                expected_path = candidate.with_name(f"{candidate.stem}-expected.png")
                expected.save(expected_path)
                differences.append(f"{error} (see {candidate.name} and {expected_path.name})")
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")
        require(not differences, "; ".join(differences))

    def sway_scaled_pixels(self):
        from PIL import Image

        scale, mode = SWAY_SCALES[self.name]
        display, ipc = self.sway('for_window [app_id=".+"] fullscreen enable\n',
                                 mode, scale)
        outputs = json.loads(self.execute([self.args.swaymsg, "-s", ipc, "-t", "get_outputs"]))
        output = next(output for output in outputs if output["name"] == "HEADLESS-1")
        logical = [output["rect"][axis] for axis in ("width", "height")]
        physical = tuple((value * scale + 50) // 100 for value in logical)
        require(abs(output["scale"] * 100 - scale) < 0.01, "wrong sway output scale")
        env = self.scene_environment()
        env.update(EMACS_WAYLAND_TEST="scaled-pixels",
                   EMACS_WAYLAND_TEST_SIZE=" ".join(map(str, physical)))
        process = self.gui(display, extra_env=env)
        self.send("scene")
        scene = self.phase("scene")
        # `image-scaling-factor' auto: the logical column width over 10,
        # at least 1, in single precision as image.c computes it; then
        # the device scale, rounded up.
        logical_column = scene[2] * 100 / scale
        factor = (struct.unpack("f", struct.pack("f", logical_column))[0] / 10
                  if logical_column > 10 else 1)
        icon = math.ceil(24 * factor * scale / 100)
        require(scene[3] == icon,
                f"tool bar icon is {scene[3]} pixels, not {icon} at {scale}%")
        explicit = math.ceil(24 * scale / 100)
        require(scene[4] == explicit,
                f"icon with :scale 1 is {scene[4]} pixels, not {explicit}")
        self.data["tool-bar-icon"] = scene[2:5]

        def pixels(path):
            with Image.open(path) as image:
                return image.size, image.convert("RGB").tobytes()

        capture_path = self.artifacts / "output.png"

        def capture():
            self.execute([self.args.grim, "-o", "HEADLESS-1", capture_path],
                         dict(self.env, WAYLAND_DISPLAY=display))
            image = pixels(capture_path)
            return image if has_rgb(image[1], (0, 126, 177)) else None

        shown = self.until(capture, "scaled scene on the output", process)
        surface, before = self.data["ready"]

        def committed_canvas():
            for path in (self.artifacts / "canvas").glob(f"surface-{surface}-*.png"):
                sequence = int(path.stem.rsplit("-", 1)[1])
                if sequence <= before:
                    continue
                try:
                    image = pixels(path)
                except OSError:
                    continue  # Emacs may still be writing the PNG.
                if has_rgb(image[1], (0, 126, 177)):
                    return path, image

        source, canvas = self.until(committed_canvas, "scaled canvas dump", process)
        require(canvas[0] == physical, "canvas dimensions disagree with scale")
        for color in ((255, 0, 0), (0, 128, 0), (0, 0, 255)):
            require(has_rgb(canvas[1], color), f"scaled scene lacks color {color}")
        shutil.copyfile(source, self.artifacts / "canvas-reference.png")
        self.data["scaled-image"] = {"logical": logical, "buffer": physical,
                                     "output": shown[0], "pixels_equal": shown == canvas}
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")
        if all(value * scale % 100 == 0 for value in logical):
            require(shown == canvas, "sway pixels differ from canvas at whole pixel size")
            reference = HERE / "references" / f"scale-{scale}.png"
            require(reference.is_file(), f"missing reviewed reference {reference.name}")
            require(canvas == pixels(reference), "scaled canvas differs from reference")

    def observe(self, display, region, runs=1, *, prefix="observer",
                samples=OBSERVER_SAMPLES, phase_ms=0, seed=1):
        """Run the observer RUNS times on REGION, in output-logical
        pixels, and return each run's latencies in ms."""
        observer = HERE / "observer" / "observer"
        require(os.access(observer, os.X_OK),
                "build the observer with make -C test/wayland/observer")
        env = dict(self.env, WAYLAND_DISPLAY=display)
        results = []
        for run in range(runs):
            output = self.execute([observer, "--output", "HEADLESS-1",
                                   "--region", ",".join(map(str, region)),
                                   "--samples", str(samples),
                                   "--settle", "150", "--phase", str(phase_ms),
                                   "--seed", str(seed)], env)
            (self.artifacts / f"{prefix}-{run}.jsonl").write_text(output + "\n")
            latencies = [json.loads(line)["latency_ns"] for line in output.splitlines()]
            require(len(latencies) == samples,
                    f"{prefix} run {run} returned {len(latencies)} of {samples} samples")
            missed = sum(1 for latency in latencies if latency < 0)
            require(not missed, f"{prefix} run {run} missed {missed} of {samples} effects")
            results.append([latency / 1e6 for latency in latencies])
        return results

    @staticmethod
    def summary(latencies):
        ordered = sorted(latencies)
        return {"p50": statistics.median(ordered),
                "p99": ordered[min(len(ordered) - 1, int(len(ordered) * 0.99))],
                "min": ordered[0], "max": ordered[-1], "samples": len(ordered)}

    def calibrate_observer(self, display, ipc):
        refclient = HERE / "observer" / "refclient"
        process = self.launch([refclient], "refclient.log",
                              dict(self.env, WAYLAND_DISPLAY=display))
        self.until(lambda: "refclient" in self.execute(
            [self.args.swaymsg, "-s", ipc, "-t", "get_tree"]),
                   "the reference client in sway", process)
        try:
            runs = self.observe(display, (100, 100, 64, 64), OBSERVER_RUNS)
        finally:
            self.stop(process)
        self.data["observer"] = [self.summary(run) for run in runs]
        medians = [run["p50"] for run in self.data["observer"]]
        spread = max(medians) - min(medians)
        self.data["p50-spread-ms"] = spread
        require(spread <= OBSERVER_SPREAD_MS,
                f"reference p50s {medians} spread {spread:.2f} ms")

    def observer_case(self):
        display, ipc = self.sway('for_window [app_id=".+"] fullscreen enable\n',
                                 OBSERVER_MODE, OBSERVER_SCALE)
        if self.name == "observer-reference":
            self.calibrate_observer(display, ipc)
            return
        env = self.scene_environment()
        process = self.gui(display, extra_env=env)
        x, y, width, height = self.data["ready"]
        scale = OBSERVER_SCALE / 100
        region = (int(x / scale), int(y / scale), int(width / scale),
                  max(1, int(height / scale)))
        run, = self.observe(display, region)
        self.data["observer"] = self.summary(run)
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")

    def perf_compositor(self, mode=PERF_MODE, scale=None, layout=PERF_FULLSCREEN):
        """Start headless sway at --perf-scale for a paired performance run
        and record its renderer, output and host.  Return the display, the
        IPC socket and the output's logical viewport."""
        scale = scale or self.args.perf_scale
        renderer = self.args.perf_renderer
        display, ipc = self.sway(layout, mode, scale, renderer, self.args.render_node)
        sway_log = self.emacs_log("sway.log")
        if renderer == "gles2":
            match = re.search(r"GL renderer: (.+)", sway_log)
            require(match and not re.search(r"llvmpipe|softpipe|swrast",
                                            match[1], re.IGNORECASE),
                    "GLES2 performance runs require a hardware renderer")
            effective = match[1]
        else:
            require("pixman renderer" in sway_log.lower(), "pixman renderer unavailable")
            effective = "pixman"
        outputs = self.performance_outputs(ipc)
        output = next(item for item in outputs if item["name"] == "HEADLESS-1")
        viewport = output["rect"]
        require(output["name"] == "HEADLESS-1" and output["scale"] == scale / 100,
                f"unexpected performance output: {output}")
        self.data["compositor"] = {
            "version": self.execute([self.args.sway, "--version"]),
            "renderer": renderer, "effective_renderer": effective,
            "render_node": str(self.args.render_node) if self.args.render_node else None,
            "output": output,
            "outputs": outputs,
        }
        self.data["host"] = timing_host()
        self.perf_sway = (layout, mode, scale, renderer, self.args.render_node)
        self.perf_ipc = ipc
        # Keep a keyboard on the seat, as on a desktop, so the frame keeps
        # focus once the observer's keyboard goes.
        self.helper("vkbd", display)
        return display, ipc, viewport

    def performance_outputs(self, ipc):
        command = [self.args.swaymsg, "-s", ipc, "-t", "get_outputs"]
        outputs = json.loads(self.execute(command))
        if self.name == FRAMES_PERF:
            primary = next(item["rect"] for item in outputs if item["name"] == "HEADLESS-1")
            x, y = primary["x"] + primary["width"], primary["y"]
            self.sway_command(ipc, f"output HEADLESS-2 pos {x} {y}")
            outputs = json.loads(self.execute(command))
            second = next(item["rect"] for item in outputs if item["name"] == "HEADLESS-2")
            require((second["x"], second["y"]) == (x, y),
                    f"unexpected multi-frame output layout: {outputs}")
        return outputs

    @staticmethod
    def cpu_clock(pid):
        clock = ctypes.c_int()
        error = ctypes.CDLL(None).clock_getcpuclockid(pid, ctypes.byref(clock))
        require(error == 0, f"could not read Emacs CPU clock: {os.strerror(error)}")
        return clock.value

    def cpu_snapshot(self, process, xserver):
        pids = {"emacs": process.pid}
        if xserver:
            pids["xwayland"] = xserver["pid"]
        return {name: time.clock_gettime_ns(self.cpu_clock(pid))
                for name, pid in pids.items()}

    def cpu_elapsed(self, process, xserver, before):
        after = self.cpu_snapshot(process, xserver)
        return {name: (end - before[name]) / 1e6 for name, end in after.items()}

    @contextmanager
    def benchmark_display(self, display, backend, prefix):
        if backend != "x":
            yield display, self.perf_ipc, None
            return

        # A separate compositor gives the X client its own server and
        # leaves the Wayland runs on a compositor with Xwayland disabled.
        with ExitStack() as stack:
            runtime = Path(stack.enter_context(tempfile.TemporaryDirectory(
                prefix="emacs-xw-", dir="/tmp")))
            wl_display, ipc, sway = self.start_sway(
                runtime, f"{prefix}-sway", *self.perf_sway, xwayland=True,
                stack=stack)

            def find_xwayland():
                for entry in Path("/proc").iterdir():
                    if not entry.name.isdecimal():
                        continue
                    pid = int(entry.name)
                    try:
                        if os.getsid(pid) != sway.pid:
                            continue
                        exe = (entry / "exe").resolve(strict=True)
                        if exe.name != "Xwayland":
                            continue
                        args = (entry / "cmdline").read_bytes().rstrip(b"\0").split(b"\0")
                    except (OSError, ProcessLookupError):
                        continue
                    return pid, exe, [os.fsdecode(arg) for arg in args]
                return None

            pid, exe, args = self.until(find_xwayland, "the benchmark Xwayland", sway)
            require(len(args) > 1 and re.fullmatch(r":[0-9]+", args[1]),
                    f"unexpected Xwayland arguments: {args}")
            version = subprocess.run([exe, "-version"], capture_output=True,
                                     text=True, timeout=self.remaining())
            require(version.returncode == 0, f"Xwayland -version: {version.stderr}")
            details = {
                "pid": pid, "display": args[1], "arguments": args,
                "version": (version.stdout + version.stderr).strip(),
            }
            if self.name == FRAMES_PERF:
                details["outputs"] = self.performance_outputs(ipc)
            self.helper("vkbd", str(wl_display), stack=stack)
            yield str(wl_display), ipc, details

    def paired_runs(self, display, measure, *, prepare=None, extra_env=None):
        """Run MEASURE on fresh Emacs processes with matched builds.
        PREPARE, if given, runs on the selected compositor before launch.
        Return the groups of backend results and the seeded generator."""
        env = self.scene_environment()
        env.update(WAYLAND_DEBUG="", EMACS_WAYLAND_TEST_SOURCE=str(SOURCE / "src/xdisp.c"))
        if self.args.perf_presentation:
            env["EMACS_WAYLAND_TEST_PRESENTATION"] = "1"
        if extra_env:
            env.update(extra_env)
        builds = {"wayland": self.args.emacs, "pgtk": self.args.pgtk_emacs}
        if self.args.x_emacs:
            builds["x"] = self.args.x_emacs
        metadata = self.data["builds"] = {}
        readies = self.data["build_details"] = {}
        pairs = self.data["pairs"] = []
        self.data.update(seed=self.args.perf_seed, timing_gate=False,
                         client_trace=self.args.perf_trace)
        rng = random.Random(self.args.perf_seed)
        first = rng.randrange(len(builds))
        backends = list(builds)
        if len(backends) == 3 and rng.randrange(2):
            backends.reverse()
        for index in range(self.args.perf_pairs):
            offset = (first + index) % len(backends)
            order = backends[offset:] + backends[:offset]
            pair = {"order": order}
            pairs.append(pair)
            for backend in order:
                prefix = f"pair-{index + 1:02}-{backend}"
                self.data["active_run"] = prefix
                output = self.data["compositor"]["output"]
                area = output["current_mode"] if backend == "wayland" else output["rect"]
                run_env = dict(env, EMACS_WAYLAND_TEST_SIZE=f"{area['width']} {area['height']}")
                with self.benchmark_display(display, backend, prefix) as (shown, ipc, xserver):
                    if xserver:
                        run_env.update(DISPLAY=xserver["display"], XAUTHORITY="/dev/null")
                    if (backend == "wayland" and self.args.perf_trace
                            and self.name in (RESIZE_PERF, *STARTUP_PERF_CASES)):
                        run_env["WAYLAND_DEBUG"] = "client"
                    if prepare:
                        prepare(shown, ipc)
                    process = self.gui(shown, builds[backend], f"{prefix}.log", run_env)
                    ready = self.data["ready"]
                    require(ready["backend"] == backend, f"{prefix} used the wrong backend")
                    if xserver:
                        require("--with-x-toolkit=no" in shlex.split(ready["options"]),
                                "the X reference must be built with --with-x-toolkit=no")
                    info = build_identity(ready)
                    if backend in metadata:
                        require(info == metadata[backend], f"{backend} build changed during run")
                    metadata[backend] = info
                    readies[backend] = ready
                    if len(metadata) == len(builds):
                        native = metadata["wayland"]
                        require(all(same_build(native, other) for other in metadata.values()),
                                f"performance builds differ in source or settings: {metadata}")
                        if "builds_verified" not in self.data:
                            self.data["builds_verified"] = verify_local_builds(
                                builds, readies, self.args.allow_unverified_builds)
                    result = measure(process, backend, prefix, ready, shown, ipc, xserver)
                    if xserver:
                        result["xwayland"] = xserver
                    pair[backend] = result
                del self.data["active_run"]
        self.data["host_after"] = timing_host()
        return pairs, rng

    def presentation_snapshot(self, process, backend):
        if backend != "wayland" or not self.args.perf_presentation:
            return {}

        def ready():
            self.send("perf-state")
            counters = self.phase("perf-state").get("presentation")
            require(counters, "the compositor lacks presentation feedback")
            return counters if not counters[3] else None

        requested, presented, discarded, _, skipped = self.until(
            ready, "complete presentation feedback", process)
        require(requested == presented + discarded and not skipped,
                "presentation feedback was incomplete")
        return {"requested": requested, "presented": presented, "discarded": discarded}

    def presentation_result(self, process, backend, before):
        if not before:
            return {}
        after = self.presentation_snapshot(process, backend)
        values = {key: after[key] - count for key, count in before.items()}
        if values["requested"]:
            values["discarded_per_1000"] = 1000 * values["discarded"] / values["requested"]
        return {"presentation": values}

    def pair_ratio(self, pairs, rng, key):
        """Record the ratio of the Wayland and PGTK medians of KEY, with a
        95% interval from resampling whole pairs."""

        def ratio(runs, baseline):
            denominator = statistics.median(run[baseline][key] for run in runs)
            if denominator:
                return statistics.median(run["wayland"][key] for run in runs) / denominator
            return None

        for baseline, suffix in (("pgtk", ""), ("x", "_x11")):
            if baseline not in pairs[0]:
                continue
            ratios = [ratio(rng.choices(pairs, k=len(pairs)), baseline)
                      for _ in range(5000)]
            name = f"{key}_ratio{suffix}"
            self.data[name] = ratio(pairs, baseline)
            if None in ratios:
                self.data[f"{name}_ci95"] = None
            else:
                ratios.sort()
                self.data[f"{name}_ci95"] = [ratios[125], ratios[4874]]

    def typing_performance(self):
        display, ipc, viewport = self.perf_compositor()
        self.calibrate_observer(display, ipc)
        # Inject at a random phase of the output's refresh, or every
        # client that commits before the next refresh measures the same.
        phase_ms = 1e6 / self.data["compositor"]["output"]["current_mode"]["refresh"]
        self.data["injection_phase_ms"] = phase_ms

        def measure(process, backend, prefix, ready, display, ipc, xserver):
            left, top, right, bottom = ready["bounds"]
            sx = viewport["width"] / ready["width"]
            sy = viewport["height"] / ready["height"]
            x, y = math.floor(left * sx), math.floor(top * sy)
            region = (x, y, math.ceil(right * sx) - x,
                      math.ceil(bottom * sy) - y)
            require(0 <= x < x + region[2] <= viewport["width"]
                    and 0 <= y < y + region[3] <= viewport["height"],
                    f"{prefix} capture region outside viewport: {region}")
            idle = self.idle_performance(process, backend, xserver)
            before = self.presentation_snapshot(process, backend)
            tracing = self.args.perf_trace and backend == "wayland"
            if tracing:
                self.send("trace-start")
                self.phase("trace-start")
            # Both runs of a pair inject at the same phases.
            pair = int(prefix.split("-")[1])
            run, = self.observe(display, region, prefix=prefix, phase_ms=phase_ms,
                                seed=self.args.perf_seed * 1000 + pair)
            trace = None
            if tracing:
                self.send("trace")
                entries = self.phase("trace")
                (self.artifacts / f"{prefix}-trace.json").write_text(json.dumps(entries))
                samples = [json.loads(line) for line in
                           (self.artifacts / f"{prefix}-0.jsonl").read_text().splitlines()]
                trace = trace_samples(samples, entries)
            if self.args.perf_idle_seconds:
                self.send("blink")
                require(self.phase("blink") is True, f"{prefix} cannot blink without focus")
                idle["blink"] = self.idle_cpu(process, backend, xserver)
            presentation = self.presentation_result(process, backend, before)
            self.send("done")
            require(self.phase("typed") == "a" * OBSERVER_SAMPLES,
                    f"{prefix} did not receive the expected keystrokes")
            self.finish(process)
            result = dict(self.summary(run), region=region, idle=idle,
                          pss_bytes=idle["memory_bytes"]["pss"],
                          samples_file=f"{prefix}-0.jsonl", **presentation)
            if self.args.perf_idle_seconds:
                result["idle_cpu_ms"] = idle["cpu_ms"]
            if trace:
                result["trace"] = trace
            return result

        pairs, rng = self.paired_runs(display, measure)
        self.pair_ratio(pairs, rng, "p50")
        self.pair_ratio(pairs, rng, "pss_bytes")
        if self.args.perf_idle_seconds:
            self.pair_ratio(pairs, rng, "idle_cpu_ms")

    def scroll_performance(self, cadence=False):
        """Time Emacs's CPU for 10 s of 120 Hz touchpad scrolling through
        the benchmark buffer with `pixel-scroll-precision-mode'.  With
        CADENCE, also capture a band of the text throughout and record
        the intervals between updates instead of gating on CPU time."""
        display, _, viewport = self.perf_compositor()
        refresh_ms = 1e6 / self.data["compositor"]["output"]["current_mode"]["refresh"]
        width, height = viewport["width"], viewport["height"]
        distance = SCROLL_STEP * SCROLL_FRAMES
        self.data["workload"] = {"frames": SCROLL_FRAMES, "hz": SCROLL_HZ,
                                 "logical_pixels": distance}

        def measure(process, backend, prefix, ready, display, ipc, xserver):
            vptr = self.helper("vptr", display)
            settings = self.data.setdefault("settings", {})
            settings[backend] = ready["settings"]
            require(len({json.dumps(value) for value in settings.values()}) == 1,
                    f"scroll settings differ: {settings}")
            # Wayland frames count physical pixels, and PGTK logical ones.
            scale = ready["width"] / width
            self.inject(vptr, f"move {width // 2} {height // 2} {width} {height}")
            factor = finger_pixels(backend, ready["window-height"], scale)
            calibration = None
            if xserver:
                factor, calibration = self.calibrate_x_scroll(vptr, scale, prefix)
            if self.name == IMAGE_SCROLL_PERF:
                self.send("image-scroll-setup")
                self.phase("image-scroll-setup")
            step = SCROLL_STEP / factor
            before = self.presentation_snapshot(process, backend)
            if cadence:
                watcher = self.watch(display, (0, height // 3, width, height // 3),
                                     1000 * SCROLL_FRAMES // SCROLL_HZ + 1000)
            tracing = cadence and self.args.perf_trace and backend == "wayland"
            if tracing:
                self.send("trace-start")
                self.phase("trace-start")
            cpu_start = self.cpu_snapshot(process, xserver)
            wall_start = time.monotonic_ns()
            reply, = self.inject(vptr, f"finger 0 {step} {SCROLL_FRAMES} {SCROLL_HZ}")
            wall_end = time.monotonic_ns()
            self.send("scrolled")
            scrolled = self.phase("scrolled")
            cpu = self.cpu_elapsed(process, xserver, cpu_start)
            wall = (time.monotonic_ns() - wall_start) / 1e9
            entries = None
            if tracing:
                self.send("trace")
                entries = self.phase("trace")
                (self.artifacts / f"{prefix}-trace.json").write_text(json.dumps(entries))
            presentation = self.presentation_result(process, backend, before)
            self.send("done")
            self.finish(process)
            self.stop(vptr)
            result = {}
            if cadence:
                watcher, output, reader = watcher
                require(watcher.wait(timeout=10) == 0, f"{prefix} observer failed")
                reader.join(timeout=10)
                (self.artifacts / f"{prefix}-cadence.jsonl").write_text("".join(output))
                lines = [json.loads(line) for line in output]
                captured = [line["captured_ns"] for line in lines if "captured_ns" in line]
                # Every changed capture arrived, and they span the stroke
                # without a stall.  The first update may lag the stroke:
                # PGTK's first came more than 100 ms in.
                require(lines and lines[-1].get("changed") == len(captured),
                        f"{prefix} observer lines are incomplete: {lines[-1:]}")
                require(captured, f"{prefix} captured no changed updates")
                first_ms = (captured[0] - wall_start) / 1e6
                last_ms = (wall_end - captured[-1]) / 1e6
                require(first_ms <= 500 and last_ms <= 100,
                        f"{prefix} captures do not cover the stroke: the first came"
                        f" {first_ms:.1f} ms after it began and the last"
                        f" {last_ms:.1f} ms before it ended")
                times = [stamp for stamp in captured if wall_start <= stamp <= wall_end]
                stall = max((b - a for a, b in zip(times, times[1:])), default=0) / 1e6
                require(stall <= 250, f"{prefix} updates stalled for {stall:.0f} ms")
                result["first_update_ms"] = first_ms
                intervals = sorted((b - a) / 1e6 for a, b in zip(times, times[1:]))
                require(len(intervals) > SCROLL_FRAMES // SCROLL_HZ,
                        f"{prefix} captured only {len(times)} updates")
                if entries:
                    result["gaps"] = trace_gaps(times, entries, 1.5 * refresh_ms)
                result = {**result, "updates": len(times),
                          "p50_interval_ms": statistics.median(intervals),
                          "p95_interval_ms": intervals[int(len(intervals) * 0.95)],
                          "max_interval_ms": intervals[-1],
                          "missed": sum(1 for i in intervals if i > 1.5 * refresh_ms),
                          "captures": lines[-1]}
            late_us = int(reply.split()[2])
            require(late_us < 1e6 / SCROLL_HZ,
                    f"{prefix}: vptr fell {late_us} us behind its schedule")
            logical = scrolled["pixels"] / scale
            require(abs(logical - distance) < scrolled["line-height"] / scale,
                    f"{prefix} scrolled {logical} logical pixels, not {distance}")
            return dict(result, cpu_ms=sum(cpu.values()), cpu_by_process_ms=cpu,
                        wall_seconds=wall, late_us=late_us,
                        finger_step=step, scroll_factor=factor,
                        scroll_calibration=calibration,
                        scrolled_pixels=scrolled["pixels"], logical_pixels=logical,
                        **presentation)

        pairs, rng = self.paired_runs(display, measure)
        if cadence:
            self.data["refresh_ms"] = refresh_ms
            self.pair_ratio(pairs, rng, "p95_interval_ms")
        else:
            self.pair_ratio(pairs, rng, "cpu_ms")

    def calibrate_x_scroll(self, pointer, scale, prefix):
        # Precision scroll rounds each X event to a whole pixel.  A
        # probe far from the target step cannot predict the rounding.
        probes = self.data.setdefault("x_scroll_calibration", {}).setdefault(prefix, [])
        low, high = 0, None
        step, count = 2.0, 360
        for _ in range(8):
            self.inject(pointer, f"finger 0 {step} {count} {SCROLL_HZ}")
            self.send("scrolled")
            probe = self.phase("scrolled")
            average = probe["pixels"] / (scale * count)
            self.send("scroll-reset")
            self.phase("scroll-reset")
            probes.append(dict(step=step, frames=count, measured_pixels=probe["pixels"],
                               pixels_per_frame=average))
            if abs(average - SCROLL_STEP) * SCROLL_FRAMES < probe["line-height"] / scale:
                return SCROLL_STEP / step, probes
            if average < SCROLL_STEP:
                low = step
            else:
                high = step
            if high is not None and low:
                step = (low + high) / 2
            elif average:
                step *= SCROLL_STEP / average
            else:
                step *= 2
        raise Failure(f"{prefix} could not calibrate X11 finger scrolling: {probes}")

    def watch(self, display, region, ms):
        """Start the observer watching REGION for MS milliseconds, wait
        for its first capture, and collect its lines on a thread, so that
        a full pipe never stops it capturing.  Return the process, the
        list of lines and the thread."""
        process = subprocess.Popen(
            [HERE / "observer" / "observer", "--output", "HEADLESS-1",
             "--region", ",".join(map(str, region)), "--watch", str(ms)],
            env=dict(self.env, WAYLAND_DISPLAY=display), stdout=subprocess.PIPE, text=True)
        self.stack.callback(self.stop, process)
        require(json.loads(process.stdout.readline()).get("ready"), "the observer failed")
        lines = []
        reader = threading.Thread(target=lambda: lines.extend(process.stdout), daemon=True)
        reader.start()
        return process, lines, reader

    def captured_updates(self, watcher, prefix):
        process, output, reader = watcher
        self.finish(process)
        reader.join(timeout=self.remaining())
        require(not reader.is_alive(), f"{prefix} capture log did not close")
        filename = f"{prefix}-captures.jsonl"
        (self.artifacts / filename).write_text("".join(output), encoding="utf-8")
        lines = [json.loads(line) for line in output]
        updates = [line for line in lines if "captured_ns" in line]
        require(lines and lines[-1].get("changed") == len(updates),
                f"{prefix} capture log is incomplete")
        return updates, filename

    def sway_command(self, ipc, command):
        response = subprocess.run([self.args.swaymsg, "-s", ipc, command], env=self.env,
                                  timeout=self.remaining(), capture_output=True, text=True)
        require(response.returncode == 0,
                f"sway rejected {command}: {response.stdout or response.stderr}")

    def perf_views(self, ipc):
        tree = json.loads(self.execute([self.args.swaymsg, "-s", ipc, "-t", "get_tree"]))
        views = []

        def visit(node, output=None):
            if node["type"] == "output":
                output = node["name"]
            if node.get("app_id") or node.get("window_properties"):
                views.append((output, node))
            for child in node["nodes"] + node["floating_nodes"]:
                visit(child, output)

        visit(tree)
        return views

    def workload_performance(self):
        mode, layout = PERF_MODE, PERF_FULLSCREEN
        if self.name == REDRAW_PERF:
            mode = "3840x2160@60Hz"
        if self.name == RESIZE_PERF:
            layout = PERF_FLOATING
        if self.name == FRAMES_PERF:
            layout = (PERF_FLOATING + "output HEADLESS-1 pos 0 0\n"
                      "output HEADLESS-2 resolution 1200x900@60Hz scale 2\n")
        display, ipc, viewport = self.perf_compositor(mode=mode, layout=layout)
        watcher = None
        extra_env = {}
        if self.name == CLIPBOARD_PERF:
            extra_env["EMACS_WAYLAND_TEST_DATA"] = str(self.runtime)
            digests = {}
            block = b"0123456789abcdef" * 4096
            for size in PERF_CLIPBOARD_SIZES:
                with (self.runtime / str(size)).open("wb") as output:
                    output.write(f"Clipboard benchmark {size:08d}\n".encode("ascii"))
                    for offset in range(output.tell(), size, len(block)):
                        output.write(block[:size - offset])
                with (self.runtime / str(size)).open("rb") as source:
                    digests[size] = hashlib.file_digest(source, "sha256").hexdigest()
        if self.name == STARTUP_INIT_PERF:
            self.perf_init = self.runtime / "init"
            self.perf_init.mkdir()
            (self.perf_init / "init.el").write_text(
                "(require 'package)\n"
                "(dolist (library '(org dired python project vc compile eglot flymake))\n"
                "  (require library))\n"
                "(global-auto-revert-mode 1)\n"
                "(global-display-line-numbers-mode 1)\n"
                "(setq wayland-test-perf-init-loaded t)\n", encoding="utf-8")
        if self.name in STARTUP_PERF_CASES:
            self.calibrate_observer(display, ipc)

        def prepare(shown, _ipc):
            nonlocal watcher
            if self.name in STARTUP_PERF_CASES:
                region = (viewport["width"] // 2 - 48, viewport["height"] // 2 - 48,
                          96, 96)
                watcher = self.watch(shown, region, 2200)

        def measure(process, backend, prefix, ready, shown, ipc, xserver):
            if self.name in STARTUP_PERF_CASES:
                captures, filename = self.captured_updates(watcher, prefix)
                require(captures and captures[0]["captured_ns"] >= self.perf_started_ns,
                        f"{prefix} did not capture startup")
                content = next((capture for capture in captures
                                if capture["center_rgb"] == PERF_STARTUP_MARKER), None)
                require(content, f"{prefix} never showed the startup marker")
                result = {"first_window_ms":
                          (captures[0]["captured_ns"] - self.perf_started_ns) / 1e6,
                          "first_content_ms":
                          (content["captured_ns"] - self.perf_started_ns) / 1e6,
                          "captures_file": filename, "emacs_init_ms": ready["init_ms"],
                          # Relates WAYLAND_DEBUG's wall-clock stamps to captures.
                          "started_ns": self.perf_started_ns,
                          "wall_minus_monotonic_ns": time.time_ns() - time.monotonic_ns()}
            elif self.name == CLIPBOARD_PERF:
                self.until(lambda: self.perf_views(ipc), "the clipboard frame map", process)
                result = self.clipboard_performance(shown, process, digests)
            elif self.name == RESIZE_PERF:
                result = self.resize_performance(shown, ipc, process, prefix)
            elif self.name == SCALE_PERF:
                result = self.scale_performance(shown, ipc, process, prefix, ready)
            elif self.name == FRAMES_PERF:
                result = self.frames_performance(ipc, process, xserver)
            else:
                canvas = self.name == CANVAS_PERF
                count = 300 if canvas else 30
                watched = self.watch(shown, (32, 32, 96, 96), 8000 if canvas else 6000)
                feedback = self.presentation_snapshot(process, backend)
                tracing = self.args.perf_trace and backend == "wayland"
                if tracing:
                    self.send("trace-start")
                    self.phase("trace-start")
                before = self.cpu_snapshot(process, xserver)
                start = time.monotonic_ns()
                self.send(f"{'canvas' if canvas else 'redraw'} {count}")
                self.phase("workload-done")
                end = time.monotonic_ns()
                cpu = self.cpu_elapsed(process, xserver, before)
                stages = {}
                if tracing:
                    self.send("trace")
                    entries = self.phase("trace")
                    del self.data["trace"]
                    (self.artifacts / f"{prefix}-trace.json").write_text(
                        json.dumps(entries))
                    stages = {"present_stages": present_stages(entries)}
                captures, filename = self.captured_updates(watched, prefix)
                updates = sum(start <= capture["captured_ns"] <= end for capture in captures)
                require(updates >= (1 if canvas else count * 0.8),
                        f"{prefix} captured only {updates} workload updates")
                seconds = (end - start) / 1e9
                result = dict(iterations=count, cpu_ms=sum(cpu.values()),
                              cpu_by_process_ms=cpu, wall_seconds=seconds,
                              updates=updates, updates_per_second=updates / seconds,
                              captures_file=filename, **stages,
                              **self.presentation_result(process, backend, feedback))
            self.send("done")
            self.finish(process)
            if (self.name == RESIZE_PERF and backend == "wayland"
                    and self.args.perf_trace):
                result["protocol_samples"] = self.resize_protocol_samples(prefix)
            if self.name == SCALE_PERF:
                self.sway_command(ipc, f"output HEADLESS-1 scale "
                                  f"{self.args.perf_scale / 100}")
            return result

        pairs, rng = self.paired_runs(display, measure, prepare=prepare,
                                      extra_env=extra_env)
        for key in ("first_window_ms", "first_content_ms", "emacs_init_ms", "cpu_ms",
                    "updates_per_second", "confirmed_visible_ms", "pss_bytes",
                    "sizes_laid_out",
                    *(f"{kind}_ms_{size}" for kind in ("yank", "serve")
                      for size in PERF_CLIPBOARD_SIZES)):
            if key in pairs[0]["wayland"]:
                self.pair_ratio(pairs, rng, key)

    def clipboard_performance(self, display, process, digests):
        env = dict(self.env, WAYLAND_DISPLAY=display)
        copy = ["wl-copy", "--foreground", "--type", PERF_CLIPBOARD_MIME]
        trials = []
        keyboard = self.helper("vkbd", display)

        def offered():
            response = subprocess.run(["wl-paste", "--list-types"], env=env,
                                      timeout=self.remaining(), capture_output=True,
                                      text=True)
            types = response.stdout.splitlines()
            self.data["clipboard_trial"]["offered_types"] = types
            return next((mime for mime in PERF_CLIPBOARD_TEXT_TYPES if mime in types),
                        None)

        for size, digest in digests.items():
            self.data["clipboard_trial"] = {"bytes": size, "direction": "to-emacs"}
            self.execute(["wl-copy", "--clear"], env)
            self.until(lambda: not offered(), "the old clipboard offer to clear", process,
                       timeout=10)
            with (self.runtime / str(size)).open("rb") as source:
                setter = subprocess.Popen(copy, stdin=source, env=env,
                                          stdout=subprocess.DEVNULL,
                                          stderr=subprocess.DEVNULL)
            try:
                self.until(offered, "the peer's clipboard offer", setter, timeout=10)
                start = time.monotonic_ns()
                self.send("yank")
                length, yank_ms = self.phase("yanked", timeout=20)
                # Includes the command's delivery, which polls in Emacs.
                yank_wall_ms = (time.monotonic_ns() - start) / 1e6
                require(length == size, f"expected {size} yanked bytes, got {length}")
                self.send("hash")
                require(self.phase("hash") == digest, "yanked clipboard contents differ")
            finally:
                self.stop(setter)

            self.until(lambda: not offered(), "the peer clipboard offer to clear", process,
                       timeout=10)
            self.data["clipboard_trial"]["direction"] = "from-emacs"
            self.send("selection-key")
            self.inject(keyboard, str(KEY["a"]))
            self.phase("selection-key")
            self.send(f"publish {size}")
            self.phase("published")
            mime = self.until(offered, "the Emacs clipboard offer", process, timeout=10)
            paste = ["wl-paste", "--no-newline", "--type", mime]
            with tempfile.TemporaryFile(dir=self.runtime) as output:
                start = time.monotonic_ns()
                reader = subprocess.run(paste, env=env, stdout=output,
                                        stderr=subprocess.PIPE, timeout=self.remaining())
                serve_ms = (time.monotonic_ns() - start) / 1e6
                require(reader.returncode == 0,
                        f"wl-paste exited {reader.returncode}: {reader.stderr!r}")
                require(output.tell() == size, f"expected {size} clipboard bytes")
                output.seek(0)
                require(hashlib.file_digest(output, "sha256").hexdigest() == digest,
                        "served clipboard contents differ")
            trials.append(dict(bytes=size, yank_ms=yank_ms, yank_wall_ms=yank_wall_ms,
                               serve_ms=serve_ms, mime=mime,
                               serve_mib_s=(size / 2**20) / (serve_ms / 1000)))
        self.stop(keyboard)
        del self.data["clipboard_trial"]
        result = {"transfers": trials}
        for trial in trials:
            result[f"yank_ms_{trial['bytes']}"] = trial["yank_ms"]
            result[f"serve_ms_{trial['bytes']}"] = trial["serve_ms"]
        return result

    def resize_performance(self, display, ipc, process, prefix):
        (_, view), = self.until(lambda: self.perf_views(ipc), "a mapped Emacs frame", process)
        target = f"[con_id={view['id']}]"
        self.send("resize-arm")
        self.phase("resize-armed")
        self.sway_command(ipc, f"{target} resize set width 420 px height 320 px")
        warmed = self.phase("resize-warmed", timeout=5)
        self.send("resize-samples")
        self.phase("resize-samples")
        watcher = self.watch(display, (0, 0, 800, 600), 4000)
        sent = []
        start = time.monotonic_ns()
        for index in range(200):
            scheduled = start + index * 1000000000 // 60
            time.sleep(max(0, (scheduled - time.monotonic_ns()) / 1e9))
            width = 420 + (index + 1) % 8 * 13
            height = 320 + (index + 1) % 8 * 7
            requested = time.monotonic_ns()
            self.sway_command(ipc, f"{target} resize set width {width} px height {height} px")
            sent.append(dict(width=width, height=height, scheduled_ns=scheduled,
                             requested_ns=requested, reply_ns=time.monotonic_ns()))
        captures, filename = self.captured_updates(watcher, prefix)
        self.send("resize-samples")
        geometry = self.phase("resize-samples")
        require(len(geometry) > 1 and captures,
                f"{prefix} recorded {len(geometry)} frame sizes and {len(captures)} updates")
        end = sent[-1]["requested_ns"]
        during = sum(start <= capture["captured_ns"] <= end for capture in captures)
        return dict(configures=sent, client_sizes=geometry, warmup_size=warmed,
                    captures_file=filename,
                    captured_updates=len(captures), sizes_laid_out=len(geometry),
                    updates_per_second=during / ((end - start) / 1e9))

    def resize_protocol_samples(self, prefix):
        """Measure the native client's configure, acknowledgement and commit."""
        log = self.emacs_log(f"{prefix}.log")
        match = re.search(r"get_xdg_surface\(new id (xdg_surface#\d+),"
                          r" (wl_surface#\d+)\)", log)
        require(match, f"{prefix} lacks the resize protocol trace")
        xdg, surface = map(re.escape, match.groups())
        messages = re.compile(
            rf"^\[(\d+):(\d+):(\d+\.\d+)\] [^\n]*?"
            rf"(?:{xdg}\.(configure|ack_configure)\((\d+)\)"
            rf"|{surface}\.(attach)\((wl_buffer#\d+|nil|\(nil\)),"
            rf"|{surface}\.(commit)\(\))",
            re.MULTILINE)
        configures = {}
        acked = None
        attached = False
        for hour, minute, second, event, serial, attach, obj, commit in messages.findall(log):
            stamp = (int(hour) * 3600 + int(minute) * 60 + float(second)) * 1000
            if event == "configure":
                configures[serial] = {"serial": int(serial), "configure_ms": stamp}
            elif event == "ack_configure":
                acked = configures[serial]
                acked["ack_ms"] = stamp
            elif attach:
                attached = obj.startswith("wl_buffer#")
            elif commit:
                if acked is not None:
                    acked.update(commit_ms=stamp, buffer_attached=attached)
                    acked = None
                attached = False
        require(any("commit_ms" in sample for sample in configures.values()),
                f"{prefix} has no acknowledged resize commits")
        return list(configures.values())

    def scale_performance(self, display, ipc, process, prefix, ready):
        self.send("geometry")
        original = self.phase("geometry")
        watcher = self.watch(display, (12, 200, 96, 96), 1800)
        start = time.monotonic_ns()
        self.sway_command(ipc, "output HEADLESS-1 scale 2")
        logical_size = [600, 450]
        width = [1200, 900] if ready["backend"] == "wayland" else logical_size

        def correct():
            self.send("geometry")
            geometry = self.phase("geometry")
            return geometry if geometry[:2] == width and geometry != original else None

        geometry = self.until(correct, "the scaled frame geometry", process)
        geometry_ns = time.monotonic_ns()
        self.send("scale-mark")
        self.phase("scale-mark")
        captures, filename = self.captured_updates(watcher, prefix)
        visible = [capture["captured_ns"] for capture in captures
                   if capture["captured_ns"] >= start]
        marked = [capture["captured_ns"] for capture in captures
                  if capture.get("center_rgb") == PERF_SCALE_MARK]
        require(visible, f"{prefix} saw no visible scale change")
        require(marked and marked[0] >= geometry_ns,
                f"{prefix} did not capture the verified scale marker")
        return dict(initial_geometry=original, geometry=geometry,
                    geometry_ms=(geometry_ns - start) / 1e6,
                    visible_ms=(visible[0] - start) / 1e6,
                    confirmed_visible_ms=(marked[0] - start) / 1e6,
                    captures_file=filename)

    def frames_performance(self, ipc, process, xserver):
        def placement():
            return {view["name"]: output for output, view in self.perf_views(ipc)
                    if (view.get("name") or "").startswith("emacs-perf-")}

        self.until(lambda: self.perf_views(ipc), "the initial frame map", process)
        started = time.monotonic_ns()
        before = self.cpu_snapshot(process, xserver)
        expected = {}
        for number in range(1, 10):
            name, output = f"emacs-perf-{number}", f"HEADLESS-{1 + number % 2}"
            self.send(f"frame {name}")
            self.phase("frame")
            self.until(lambda: name in placement(), f"{name} map", process)
            self.sway_command(ipc, f'[title="^{name}$"] move container to output {output}')
            expected[name] = output
        mapped = self.until(lambda: placement() == expected and expected,
                            "the expected placement of Emacs frames", process)
        views = self.perf_views(ipc)
        require(len(views) == 10 and len({output for output, _ in views}) == 2,
                f"unexpected frame count or outputs: {views}")
        cpu = self.cpu_elapsed(process, xserver, before)
        seconds = (time.monotonic_ns() - started) / 1e9
        by_process = {"emacs": self.memory_bytes(process.pid)}
        if xserver:
            by_process["xwayland"] = self.memory_bytes(xserver["pid"])
        return dict(placements=mapped, wall_seconds=seconds, cpu_ms=sum(cpu.values()),
                    cpu_by_process_ms=cpu, memory_by_process_bytes=by_process,
                    pss_bytes=sum(values["pss"] for values in by_process.values()))

    @staticmethod
    def memory_bytes(pid):
        fields = {}
        for line in Path(f"/proc/{pid}/smaps_rollup").read_text().splitlines():
            name, _, value = line.partition(":")
            if name in ("Pss", "Rss", "Private_Clean", "Private_Dirty"):
                fields[name] = int(value.split()[0]) * 1024
        return {"pss": fields["Pss"], "rss": fields["Rss"],
                "uss": fields["Private_Clean"] + fields["Private_Dirty"]}

    def idle_performance(self, process, backend, xserver):
        # Let the headless output settle before reading its process memory.
        time.sleep(0.25)
        self.send("perf-state")
        before = self.phase("perf-state")
        by_process = {"emacs": self.memory_bytes(process.pid)}
        if xserver:
            by_process["xwayland"] = self.memory_bytes(xserver["pid"])
        memory = {key: sum(fields[key] for fields in by_process.values())
                  for key in by_process["emacs"]}
        if backend == "wayland":
            memory.update(pool_bytes=before["pool_bytes"],
                          retired_bytes=before["retired_bytes"])
        result = {"memory_bytes": memory, "memory_by_process_bytes": by_process}
        if self.args.perf_idle_seconds:
            result.update(self.idle_cpu(process, backend, xserver, before))
            if backend == "wayland":
                require(result["commits"] == 0, f"native Wayland committed while idle: {result}")
        return result

    def idle_cpu(self, process, backend, xserver, before=None):
        """Measure Emacs's CPU time over the idle interval, without
        screencopy, and native Wayland's commits during it."""
        if before is None:
            self.send("perf-state")
            before = self.phase("perf-state")
        start_cpu = self.cpu_snapshot(process, xserver)
        start_wall = time.monotonic_ns()
        time.sleep(self.args.perf_idle_seconds)
        cpu = self.cpu_elapsed(process, xserver, start_cpu)
        result = {"wall_seconds": (time.monotonic_ns() - start_wall) / 1e9,
                  "cpu_ms": sum(cpu.values()), "cpu_by_process_ms": cpu}
        self.send("perf-state")
        after = self.phase("perf-state")
        if backend == "wayland":
            result["commits"] = after["commits"] - before["commits"]
        return result

    def soak_checkpoint(self, process, checkpoints, mock=False):
        """Record a quiet checkpoint: Emacs's own counts after a garbage
        collection, its PSS and its open descriptors."""
        self.send("checkpoint")
        state = self.phase("checkpoint")
        rollup = Path(f"/proc/{process.pid}/smaps_rollup").read_text()
        state["pss"] = int(re.search(r"^Pss:\s+(\d+)", rollup, re.M)[1]) * 1024
        state["fds"] = len(os.listdir(f"/proc/{process.pid}/fd"))
        state["elapsed"] = round(time.monotonic() - self.soak_start, 1)
        if mock:
            # The checkpoint's own requests, such as the child's
            # destruction, have reached mockcomp once commits stop.
            self.quiesce(process)
            state["resources"] = self.status()["resources"]
        checkpoints.append(state)
        return state

    def soak_choices(self, actions, weights):
        """The soak's actions and weights, limited to --soak-actions."""
        wanted = self.args.soak_actions
        pairs = [(action, weight) for action, weight in zip(actions, weights)
                 if not wanted or action in wanted]
        require(pairs, f"no soak actions among {actions}")
        return [action for action, _ in pairs], [weight for _, weight in pairs]

    def soak_flat(self, checkpoints):
        """Require resources after warm-up to stay flat.  Counts must not
        drift.  Memory churns as caches for each scale fill and empty,
        so compare the medians of the early and late halves of the
        settled checkpoints."""
        warm = max(2, len(checkpoints) // 4)
        require(len(checkpoints) >= warm + 4,
                f"only {len(checkpoints)} checkpoints; lengthen the soak")
        settled = checkpoints[warm:]
        first, last = settled[0], settled[-1]
        for key in ("frames", "buffers", "processes", "fds"):
            require(max(c[key] for c in settled) - min(c[key] for c in settled) <= 1,
                    f"{key} drifted: {[c[key] for c in settled]}")
        for key in ("pool_bytes", "charged_pool_bytes"):
            require(last[key] <= first[key], f"{key} grew: {[c[key] for c in settled]}")
        half = len(settled) // 2
        # Live Wayland objects by interface: a count may vary with
        # buffers and callbacks in flight, but the late checkpoints must
        # not exceed the early ones.
        if "resources" in settled[0]:
            names = set().union(*(c["resources"] for c in settled))
            for name in sorted(names):
                counts = [c["resources"].get(name, 0) for c in settled]
                require(max(counts[half:]) <= max(counts[:half]),
                        f"live {name} objects grew: {counts}")
        for key, ratio, slack in (("pss", 1.1, 8 * 2**20), ("lisp_bytes", 1.05, 2**20)):
            early = statistics.median(c[key] for c in settled[:half])
            late = statistics.median(c[key] for c in settled[half:])
            require(late <= early * ratio + slack,
                    f"{key} grew from a median of {early / 2**20:.1f} MiB to"
                    f" {late / 2**20:.1f} MiB: {[c[key] // 1024 for c in settled]} KiB")

    def soak_storm(self, process):
        """Seeded storms of input and compositor faults on mockcomp."""
        rng = random.Random(self.args.soak_seed)
        surface, _ = self.data["ready"]
        trace = (self.artifacts / "soak-trace.jsonl").open("w")
        checkpoints = self.data["checkpoints"] = []
        self.data["seed"] = self.args.soak_seed
        self.soak_start = time.monotonic()
        deadline = self.soak_start + self.args.soak_seconds
        clock = iter(range(10000, 10**12, 10000))
        # The second output starts absent; toggling it is the hotplug.
        state = {"held": False, "callbacks_off": False, "second_output": False}

        def act(kind, *commands):
            trace.write(json.dumps({"t": round(time.monotonic() - self.soak_start, 3),
                                    "action": kind, "commands": commands}) + "\n")
            for command in commands:
                if command.startswith("emacs "):
                    self.send(command[6:])
                    self.phase(command[6:])
                else:
                    self.control(command)

        def reset():
            act("reset", "hold-buffers off", "frame-callbacks on",
                *(["output remove"] if state["second_output"] else []),
                f"preferred-scale {surface} 120 1", "configure 800 600")
            state.update(held=False, callbacks_off=False, second_output=False)
            self.quiesce(process)

        def toggle(name, on, off):
            state[name] = not state[name]
            return on if state[name] else off

        self.control(f"focus {surface}")
        self.control(f"pointer enter {surface} 100.5 100.5")
        reset()
        self.soak_checkpoint(process, checkpoints, mock=True)
        next_checkpoint = time.monotonic() + self.args.soak_interval
        while time.monotonic() < deadline:
            choice = rng.choices(*self.soak_choices(
                ["type", "wheel", "finger", "configure", "scale", "hold", "callbacks",
                 "output", "child"], [5, 3, 3, 2, 2, 1, 1, 1, 2]))[0]
            if choice == "type":
                codes = [rng.choice(list(KEY_LETTERS.values()))
                         for _ in range(rng.randint(1, 20))]
                act(choice, *[f"key {code} {press}" for code in codes
                              for press in ("press", "release")])
            elif choice == "wheel":
                sign = rng.choice((-1, 1))
                act(choice, f"clock {next(clock)}",
                    f"pointer axis vertical {15 * sign} wheel {120 * sign}")
            elif choice == "finger":
                step = rng.uniform(-8, 8)
                act(choice, *[f"pointer axis vertical {step:.2f} finger"
                              for _ in range(rng.randint(2, 12))],
                    "pointer stop vertical")
            elif choice == "configure":
                act(choice, "configure " + " ".join(
                    f"{rng.randint(200, 1600)} {rng.randint(150, 1000)}"
                    for _ in range(rng.randint(1, 8))))
            elif choice == "scale":
                scale = rng.choice((120, 150, 180, 240))
                act(choice, f"preferred-scale {surface} {scale} {-(-scale // 120)}")
            elif choice == "hold":
                act(choice, f"hold-buffers {toggle('held', 'on', 'off')}")
            elif choice == "callbacks":
                act(choice, f"frame-callbacks {toggle('callbacks_off', 'off', 'on')}")
            elif choice == "output":
                act(choice, f"output {toggle('second_output', 'add', 'remove')}")
            else:
                act(choice, f"emacs child-{rng.choice(('show', 'hide'))}")
            time.sleep(rng.uniform(0, 0.05))
            if time.monotonic() >= next_checkpoint:
                reset()
                self.soak_checkpoint(process, checkpoints, mock=True)
                next_checkpoint = time.monotonic() + self.args.soak_interval
        reset()
        self.soak_checkpoint(process, checkpoints, mock=True)
        require(not self.status()["violation_total"], "protocol violations during the soak")
        self.soak_flat(checkpoints)
        # The storm ends with the compositor going away.
        act("disconnect", "disconnect")
        trace.close()

    def soak_sway(self):
        """Everyday use on headless sway: typing, scrolling, resizing,
        scale changes, child frames and clipboard transfers."""
        rng = random.Random(self.args.soak_seed)
        display, ipc = self.sway('for_window [app_id=".+"] floating enable\n', "1600x1000")
        helpers = {name: self.helper(name, display) for name in ("vkbd", "vptr")}
        process = self.gui(display, extra_env=self.scene_environment())
        env = dict(self.env, WAYLAND_DISPLAY=display)
        trace = (self.artifacts / "soak-trace.jsonl").open("w")
        checkpoints = self.data["checkpoints"] = []
        self.data["seed"] = self.args.soak_seed
        self.soak_start = time.monotonic()
        deadline = self.soak_start + self.args.soak_seconds

        def act(kind, detail):
            trace.write(json.dumps({"t": round(time.monotonic() - self.soak_start, 3),
                                    "action": kind, "detail": detail}) + "\n")

        def sway(command):
            self.execute([self.args.swaymsg, "-s", ipc, command])

        def reset():
            sway("output HEADLESS-1 scale 1")
            sway('[app_id="emacs"] resize set 1000 700')
            time.sleep(0.5)

        sway('[app_id="emacs"] focus')
        reset()
        self.soak_checkpoint(process, checkpoints)
        next_checkpoint = time.monotonic() + self.args.soak_interval
        while time.monotonic() < deadline:
            choice = rng.choices(*self.soak_choices(
                ["type", "scroll", "resize", "scale", "child", "copy", "paste"],
                [5, 4, 2, 1, 2, 1, 1]))[0]
            if choice == "type":
                codes = " ".join(str(rng.choice(list(KEY_LETTERS.values())))
                                 for _ in range(rng.randint(1, 30)))
                act(choice, codes)
                self.inject(helpers["vkbd"], codes)
            elif choice == "scroll":
                command = (f"finger 0 {rng.uniform(-6, 6):.2f} {rng.randint(10, 120)} 120"
                           if rng.random() < 0.7 else f"wheel 0 {rng.choice((-3, -1, 1, 3))}")
                act(choice, command)
                self.inject(helpers["vptr"], "move 500 350 1600 1000", command)
            elif choice == "resize":
                size = f"{rng.randint(400, 1500)} {rng.randint(300, 950)}"
                act(choice, size)
                sway(f'[app_id="emacs"] resize set {size}')
            elif choice == "scale":
                scale = rng.choice(("1", "1.25", "1.5", "2"))
                act(choice, scale)
                sway(f"output HEADLESS-1 scale {scale}")
            elif choice == "child":
                command = f"child-{rng.choice(('show', 'hide'))}"
                act(choice, command)
                self.send(command)
                self.phase(command)
            elif choice == "copy":
                text = f"soak {rng.randrange(10**9)}"
                act(choice, text)
                # Taking the selection needs a fresh input serial, as a
                # copy command bound to a key has: wait until Emacs has
                # read the key.
                self.inject(helpers["vkbd"], str(KEY_LETTERS["a"]))
                time.sleep(0.2)
                self.send(f"copy {text}")
                require(self.phase("copy") is True, "Emacs could not take the clipboard")
                pasted = self.execute(["wl-paste", "--no-newline"], env)
                require(pasted == text, f"wl-paste read {pasted!r}, not {text!r}")
            else:
                text = f"soak {rng.randrange(10**9)}"
                act(choice, text)
                subprocess.run(["wl-copy", text], env=env, check=True,
                               timeout=self.remaining())
                self.send("paste")
                require(self.phase("paste") == text, f"Emacs did not read {text!r}")
            time.sleep(rng.uniform(0, 0.1))
            if time.monotonic() >= next_checkpoint:
                reset()
                self.soak_checkpoint(process, checkpoints)
                next_checkpoint = time.monotonic() + self.args.soak_interval
        reset()
        self.soak_checkpoint(process, checkpoints)
        trace.close()
        self.soak_flat(checkpoints)
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")

    def helper(self, name, display, *, stack=None):
        """Start the input helper NAME from observer/ on DISPLAY."""
        process = subprocess.Popen([HERE / "observer" / name],
                                   env=dict(self.env, WAYLAND_DISPLAY=display),
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        (stack if stack is not None else self.stack).callback(self.stop, process)
        require(process.stdout.readline().strip() == "ready", f"{name} failed")
        return process

    @staticmethod
    def inject(helper, *commands):
        """Send COMMANDS to HELPER in turn and return its replies."""
        replies = []
        for command in commands:
            helper.stdin.write(command + "\n")
            helper.stdin.flush()
            reply = helper.stdout.readline().strip()
            require(reply.startswith("ok"), f"{helper.args[0].name} {command!r}: {reply!r}")
            replies.append(reply)
        return replies

    def input_parity(self):
        """Send the same keyboard and pointer input to Wayland and PGTK
        on headless sway, and compare the Emacs events each reads."""
        wtype = shutil.which("wtype")
        require(wtype, "wtype is required for input parity")
        display, _ = self.sway('for_window [app_id=".+"] fullscreen enable\n')
        env = dict(self.env, WAYLAND_DISPLAY=display)
        # Keep a keyboard on the headless seat while wtype switches and
        # releases its temporary keymap for each client, and give the seat
        # its pointer before Emacs binds it.
        helpers = {name: self.helper(name, display) for name in ("vkbd", "vptr")}

        def inject(helper, *commands):
            self.inject(helpers[helper], *commands)

        def point(x, y):
            return f"move {x} {y} 1024 768"

        observations = self.data["events"] = {}
        for backend, emacs in (("wayland", self.args.emacs), ("pgtk", self.args.pgtk_emacs)):
            log = f"{backend}.log"
            process = self.gui(display, emacs, log, self.scene_environment())
            ready = self.data.setdefault("frames", {})[backend] = self.data["ready"]
            require(ready["backend"] == backend, f"{log} used the wrong backend")
            self.until(lambda: re.search(r"wl_keyboard(?:#|@)\d+\.enter\(",
                                         self.emacs_log(log)),
                       f"{backend} keyboard focus", process)
            self.execute([wtype, "-k", "a",
                          "-M", "shift", "-k", "F8", "-m", "shift",
                          "-M", "ctrl", "-k", "a", "-m", "ctrl", "-k", "F8",
                          "-k", "dead_acute", "-k", "e"], env)
            self.send("keys")
            keys = self.phase("keys")
            inject("vptr", point(205, 110), f"click {BTN_LEFT}", f"click {BTN_LEFT}")
            # Let the double click expire before the next left press.
            time.sleep(0.6)
            inject("vptr", point(605, 330), f"click {BTN_MIDDLE}", f"click {BTN_RIGHT}",
                   point(105, 220), f"press {BTN_LEFT}", point(405, 280),
                   f"release {BTN_LEFT}", point(205, 110))
            inject("vkbd", f"+{KEY['shift']}")
            inject("vptr", f"click {BTN_LEFT}")
            inject("vkbd", f"-{KEY['shift']}")
            inject("vptr", point(505, 440), "wheel 0 1", "wheel 0 -1", "wheel 1 1",
                   f"click {BTN_SIDE}")
            time.sleep(0.6)
            inject("vptr", f"click {BTN_BACK}", f"finger 0 {FINGER_STEP} {FINGER_FRAMES} 120",
                   f"finger 1 {FINGER_STEP} {FINGER_FRAMES} 120")
            self.send("pointer")
            pointer = self.phase("pointer")
            inject("vptr", f"finger 0 {FINGER_STEP} 8 120")
            self.send("precision")
            self.data.setdefault("pixel_deltas", {})[backend] = self.phase("precision")
            observations[backend] = {"keys": keys, "pointer": pointer}
            self.send("done")
            self.finish(process)
            require(not self.protocol_errors(log), f"{backend} protocol error")
        # Intentional differences, where we follow X.  GTK numbers
        # BTN_BACK 11, after the X server; we make it back, like BTN_SIDE.
        # PGTK names scrolling right wheel-left.  Both send an event per
        # 10 compositor pixels of finger travel, but each carries its own
        # remainder, so compare each finger stroke by its direction.
        streams = {}
        swap = {"wheel-left": "wheel-right", "wheel-right": "wheel-left"}
        for backend, observed in observations.items():
            events, strokes = [], []
            for event in observed["pointer"]:
                if backend == "pgtk":
                    event = re.sub(r"wheel-(?:left|right)",
                                   lambda m: swap[m[0]], event.replace("mouse-11", "mouse-8"))
                wheel = re.fullmatch(r"<(double-|triple-)?(wheel-\w+)>", event)
                if wheel and wheel[1]:
                    # A repeat within a stroke.
                    events[-1] = f"<{wheel[2]}>..."
                    strokes[-1] += 1
                else:
                    if wheel:
                        strokes.append(1)
                    events.append(event)
            streams[backend] = {"keys": observed["keys"], "pointer": events}
            self.data.setdefault("wheel_counts", {})[backend] = strokes
        require(streams["wayland"] == streams["pgtk"],
                f"Wayland and PGTK input differ: {streams}")
        require(streams["wayland"]["keys"] == ["a", "S-<f8>", "C-a", "<f8>", "é"],
                f"the key sequence was not delivered: {streams}")
        names = [re.sub(r"@.*", "", event) for event in streams["wayland"]["pointer"]]
        require(names == ["<down-mouse-1>", "<mouse-1>", "<double-down-mouse-1>",
                          "<double-mouse-1>", "<down-mouse-2>", "<mouse-2>",
                          "<down-mouse-3>", "<mouse-3>", "<down-mouse-1>",
                          "<drag-mouse-1>", "S-<down-mouse-1>", "S-<mouse-1>",
                          "<wheel-down>", "<wheel-up>", "<wheel-right>",
                          "<down-mouse-8>", "<mouse-8>", "<down-mouse-8>", "<mouse-8>",
                          "<wheel-down>...", "<touch-end>", "<wheel-right>...",
                          "<touch-end>"],
                f"the pointer sequence was not delivered: {names}")
        # As on X, a stroke sends whole scroll units of 10 compositor
        # pixels, each moving the 2/3 power of the window's height.  Axis
        # frames the compositor delivers together become one event of
        # several units, as Xwayland's single XI2 motion does on X.
        ready = self.data["frames"]["wayland"]
        units = FINGER_STEP * FINGER_FRAMES / 10
        require(self.data["wheel_counts"]["wayland"][-2:] == [int(units)] * 2,
                f"finger strokes gave {self.data['wheel_counts']} for {ready}")
        delta = -FINGER_STEP / 10 * ready["window-height"] ** (2 / 3)
        ratios = [value / delta for value in self.data["pixel_deltas"]["wayland"]]
        require(all(round(ratio) >= 1 and abs(ratio - round(ratio)) < 1e-6
                    for ratio in ratios)
                and round(sum(ratios)) == 8,
                f"pixel deltas were {self.data['pixel_deltas']}, not 8 units of {delta}")

    def fcitx5(self):
        """Type Hangul through fcitx5 into Emacs: g k s w k composes
        한 and then 자, and Space commits the second syllable."""
        fcitx5 = os.environ.get("EMACS_WAYLAND_TEST_FCITX5")
        hangul = os.environ.get("EMACS_WAYLAND_TEST_FCITX5_HANGUL")
        require(fcitx5 and hangul, "fcitx5 and fcitx5-hangul are required")
        display, _ = self.sway()
        # A keyboard exists before the input method grabs it, as with a
        # real one: one virtual keyboard for the whole scenario.
        keyboard = subprocess.Popen(
            [HERE / "observer" / "vkbd"], env=dict(self.env, WAYLAND_DISPLAY=display),
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        self.stack.callback(self.stop, keyboard)
        require(keyboard.stdout.readline().strip() == "ready", "vkbd failed")

        def type_keys(*keys):
            keyboard.stdin.write(" ".join(str(KEY[key]) for key in keys) + "\n")
            keyboard.stdin.flush()
            require(keyboard.stdout.readline().strip() == "ok", "vkbd failed")

        config = self.runtime / "config" / "fcitx5"
        config.mkdir(parents=True)
        (config / "profile").write_text(FCITX5_PROFILE)
        (config / "config").write_text("[Behavior]\nActiveByDefault=True\n")
        env = dict(self.env, WAYLAND_DISPLAY=display,
                   XDG_CONFIG_HOME=str(self.runtime / "config"),
                   FCITX_ADDON_DIRS=f"{hangul}/lib/fcitx5:{fcitx5}/lib/fcitx5",
                   # Hangul's keyboards and fcitx5's layouts come from
                   # other packages on the existing path.
                   XDG_DATA_DIRS=":".join(
                       filter(None, (f"{hangul}/share", f"{fcitx5}/share",
                                     self.env.get("XDG_DATA_DIRS")))))
        env.pop("DBUS_SESSION_BUS_ADDRESS", None)
        method = self.launch([f"{fcitx5}/bin/fcitx5", "--disable=all",
                              "--enable=wayland,waylandim,hangul,keyboard",
                              "--verbose=" + os.environ.get("FCITX5_VERBOSE",
                                                            "default=4")],
                             "fcitx5.log", env)
        self.until(lambda: "Loaded addon waylandim"
                   in (self.artifacts / "fcitx5.log").read_text(errors="replace"),
                   "fcitx5's input method", method)
        process = self.gui(display)
        results = self.data["fcitx5"] = {}

        # fcitx5 grabs the keyboard some time after it loads.  Until
        # then, g reaches Emacs as a key; once it grabs, g starts the
        # syllable and no key arrives.
        def grabbing():
            type_keys("g")
            if self.read_keys() == "nil":
                return True
            time.sleep(0.5)
            return False

        self.until(grabbing, "fcitx5 grabbing the keyboard", process)
        preedit = results["probe"] = self.preedit()
        require(preedit == "ㅎ", f"g showed the preedit {preedit!r}")
        type_keys("k", "s", "w", "k")
        got = results["first"] = self.read_keys()
        require(got == "한", f"the first syllable committed {got!r}")
        preedit = results["preedit"] = self.preedit()
        require(preedit == "자", f"the second syllable showed {preedit!r}")
        type_keys("space")
        got = results["second"] = self.read_keys(2)
        require(got == "자 SPC", f"Space gave {got!r}")
        require(self.preedit() == "", "the preedit stayed after the commit")
        self.send("done")
        self.finish(process)
        require(not self.protocol_errors(), "unexpected sway protocol error")

    def run(self):
        if self.name == INPUT_PARITY:
            self.input_parity()
            return
        if self.name == SOAK_SWAY:
            self.soak_sway()
            return
        if self.name == TYPING_PERF:
            self.typing_performance()
            return
        if self.name in SCROLL_PERF_CASES:
            self.scroll_performance(self.name == SCROLL_CADENCE)
            return
        if self.name in PERF_WORKLOADS:
            self.workload_performance()
            return
        if self.name == FCITX5_CASE:
            self.fcitx5()
            return
        if self.name in OBSERVER_CASES:
            self.observer_case()
            return
        if self.name == SELECTION_SWAY_CASE:
            self.sway_clipboard()
            return
        if self.name == INSTRUCTIONS_CASE:
            self.redisplay_instructions()
            return
        if self.name.startswith("sway-"):
            if self.name == PARITY_CASE:
                display, _ = self.sway('for_window [app_id=".+"] fullscreen enable\n')
                self.pixel_parity(display)
                return
            if self.name in SWAY_SCALES:
                self.sway_scaled_pixels()
                return
            if self.name == IMAGE_PARITY:
                display, _ = self.sway('for_window [app_id=".+"] fullscreen enable\n')
                self.image_parity(display)
                return
            if self.name in IMAGE_SCALES:
                self.sway_scaled_images()
                return
            if self.name in CHILD_SWAY_SCALES:
                self.sway_child()
                return
            display, ipc = self.sway()
            if self.name == "sway-daemon":
                self.daemon(display, ipc)
            else:
                process = self.gui(display)
                self.sway_mapped(ipc, process)
                self.send("done")
                self.finish(process)
            require(not self.protocol_errors(), "unexpected sway protocol error")
            return

        display = self.mock()
        if self.name == "daemon":
            self.daemon(display)
            self.finish_mocks()
            require(not self.protocol_errors(), "unexpected mockcomp protocol error")
            return
        if self.name in SURVIVOR_CASES:
            self.mock("survivor")
        if self.name in ("backpressure-input", "write-only"):
            # Resuming reads must not produce a frame or buffer event to
            # drive the input side of the loop.
            self.control("frame-callbacks off")
            self.control("hold-buffers on")
        if self.name in ("interrupted-redraw", "redisplay-loss"):
            # Discarding a partial redisplay and losing the display during
            # drawing both require the previous frame callback to stay pending.
            self.control("frame-callbacks off")
        if self.name in ("pool-lifetime", "damage-repair", "damage-repair-rgb"):
            self.control("hold-buffers on")

        extra_env = None
        if self.name == DESKTOP_CASE:
            extra_env = dict(self.scene_environment(),
                             DBUS_SESSION_BUS_ADDRESS=self.session_bus())
        process = self.gui(display, extra_env=extra_env)
        self.until(lambda: self.status()["mapped"] >= 1, "initial frame map", process)
        if self.name in SURVIVOR_CASES:
            self.until(lambda: self.status("survivor")["mapped"] == 1,
                       "the surviving frame to map", process)
        match self.name:
            case "startup":
                self.send("done")
            case "startup-background":
                # The background buffer mapped the frame during Lisp, and
                # the first image is the second buffer attached.
                self.until(lambda: self.status()["content_commits"] >= 2,
                           "the first image to replace the background", process)
                self.send("done")
            case "quiet-ping" | "busy-lisp":
                if self.name == "busy-lisp":
                    self.send("go")
                    self.phase("busy")
                self.control("ping")
                self.until(lambda: self.status()["pongs"] == 1, "pong", process)
                if self.name == "quiet-ping":
                    self.send("done")
                    self.phase("metrics")
                else:
                    self.phase("metrics")
                    self.send("done")
            case "backpressure-input" | "write-only":
                self.backpressure(process)
            case "idle-loss":
                self.control("disconnect")
            case "thread-selection" | "thread-loss":
                if self.name == "thread-selection":
                    surface, _ = self.data["ready"]
                    self.control(f"focus {surface}")
                    self.tap("a")
                    self.send("publish")
                    self.phase("published")
                    self.until(lambda: self.status()["selections_set"] == 1,
                               "test selection published", process)
                self.send("go")
                self.phase("waiting")
                if self.name == "thread-selection":
                    self.selection_request(process, "text/plain;charset=utf-8")
                    self.send("done")
                else:
                    self.control("disconnect")
            case "redisplay-loss":
                self.send("draw")
                self.phase("drawing-next")
                self.control("disconnect")
                (self.runtime / "resume").touch()
            case "commit-loss":
                self.control("disconnect-on-commit")
                self.send("draw")
            case "pixel-baseline" | "glyph-pixels":
                self.pixel_baseline(process)
            case "interrupted-redraw":
                self.interrupted_redraw(process)
            case "pool-lifetime":
                self.pool_lifetime(process)
                self.check_budgets()
            case "damage-repair":
                self.damage_repair(process)
                self.check_budgets()
            case "damage-repair-rgb":
                self.damage_repair(process)
            case "subpixel-redraw":
                self.subpixel_redraw(process)
            case "pool-budget":
                self.pool_budget(process)
                self.check_budgets()
            case "pool-budget-recovery":
                self.pool_budget(process)
            case "mid-draw":
                self.mid_draw(process)
            case "mid-redisplay-write":
                self.mid_redisplay_write(process)
            case "frame-states":
                self.frame_states(process)
            case "keyboard":
                self.keyboard(process)
            case "key-repeat":
                self.key_repeat(process)
            case "seat-changes":
                self.seat_changes(process)
            case "input-queue":
                self.input_queue(process)
            case "quit":
                self.quit_keys(process)
            case "pointer":
                self.pointer(process)
            case "pointer-theme":
                self.pointer_theme(process)
            case "scroll" | "scroll-v5" | "scroll-v4":
                self.scroll(process)
            case "input-method":
                self.input_method(process)
            case case_name if case_name == DESKTOP_CASE:
                self.send("done")
            case "minimal-protocols":
                self.minimal_protocols(process)
            case case_name if case_name == SOAK_STORM:
                self.soak_storm(process)
            case "child-frames":
                self.child_frames(process)
            case "child-hierarchy":
                self.child_hierarchy(process)
            case "child-pointer":
                self.child_pointer(process)
            case "child-drag":
                self.child_drag(process)
            case "child-no-subcompositor":
                self.send("create")
                self.phase("rejected")
                require(not self.status()["subsurfaces"], "unexpected child surface")
                self.send("done")
            case child_case if child_case in CHILD_SCALE_CASES:
                self.child_scale(process)
            case "child-mixed-scale":
                self.child_mixed_scale(process)
            case case_name if case_name == SELECTION_CASE:
                self.selection(process)
            case case_name if case_name == MENU_CASE:
                self.send("done")
            case case_name if case_name == ACTIVATION_CASE:
                self.activation(process)
            case case_name if case_name in SCALE_CASES:
                self.scale_frames(process)
            case "pre-redisplay-loss" | "queued-quit":
                self.send("go")
                self.phase("redisplaying")
                self.control("disconnect")
                self.phase("guarded")
                self.send("leave")
                self.phase("leaving")
            case "epipe":
                self.send("go")
                self.phase("flushing")
                self.control("protocol-error-epipe")
                (self.runtime / "resume").touch()
                self.phase("metrics")
            case "overflow":
                self.control("stall on")
                self.send("fill")
                # Keep reads stalled until Emacs detects the buffer cap.
        self.finish(process, 70 if self.name in ("idle-loss", SOAK_STORM) else 0)
        if self.name == "epipe":
            require(re.search(r"emacs: Wayland protocol error 0 on xdg_wm_base@\d+",
                              self.emacs_log()), "missing protocol error identity after EPIPE")
        else:
            require(not self.protocol_errors(), "unexpected mockcomp protocol error")
        if self.name in SURVIVOR_CASES or self.name in ("idle-loss", SOAK_STORM):
            require("Connection to the Wayland compositor lost:" in self.emacs_log()
                    or self.name == "epipe", "missing connection-loss diagnostic")
        if self.name == "redisplay-loss":
            require("Wayland test detected loss during glyph drawing" in self.emacs_log(),
                    "connection loss was not observed during the glyph draw")
        if self.name == "commit-loss":
            trace = (self.artifacts / "primary.trace").read_text(encoding="utf-8")
            require("## disconnect on content commit" in trace,
                    "mockcomp did not disconnect at a content commit")
        self.finish_mocks()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cases", nargs="*", metavar="CASE", help="default: run the M0 gate")
    parser.add_argument("--build-dir", type=Path, default=SOURCE,
                        help="configured Wayland build directory")
    parser.add_argument("--pgtk-build-dir", type=Path,
                        help="PGTK build of the same source tree for pixel parity")
    parser.add_argument("--x-build-dir", type=Path,
                        help="matched no-toolkit X11 build for performance comparisons")
    parser.add_argument("--mockcomp", type=Path, default=HERE / "mockcomp/mockcomp")
    parser.add_argument("--sway", default=shutil.which("sway"))
    parser.add_argument("--swaymsg", default=shutil.which("swaymsg"))
    parser.add_argument("--grim", default=shutil.which("grim"))
    parser.add_argument("--fonts", type=Path,
                        default=os.environ.get("EMACS_WAYLAND_TEST_FONTS"),
                        help="directory of the pinned DejaVu fonts")
    parser.add_argument("--timeout", type=float, default=20, help="seconds per scenario")
    parser.add_argument("--timeout-scale", type=float, default=1,
                        help="multiply every scenario deadline, as for Valgrind")
    parser.add_argument("--wrapper", help="command to run the Wayland Emacs under,"
                        " such as \"valgrind --error-exitcode=99\"")
    parser.add_argument("--output", type=Path,
                        default=os.environ.get("EMACS_WAYLAND_TEST_OUTPUT",
                                               HERE / "artifacts"),
                        help="parent directory for results and logs")
    parser.add_argument("--list", action="store_true", help="list scenarios and exit")
    parser.add_argument("--record-budgets", action="store_true",
                        help="write measured counters plus 20%%, or instruction"
                        " counts plus 1%%, to budgets.json")
    parser.add_argument("--perf-renderer", choices=("pixman", "gles2"), default="pixman",
                        help="sway renderer for the paired performance pilots")
    parser.add_argument("--render-node", type=Path,
                        help="GPU render node for GLES2 performance runs")
    parser.add_argument("--perf-scale", type=int, choices=(100, 150),
                        default=OBSERVER_SCALE,
                        help="output scale of the performance runs, in percent")
    parser.add_argument("--perf-pairs", type=int, default=3,
                        help="number of paired performance runs (default: 3)")
    parser.add_argument("--perf-seed", type=int, default=0,
                        help="seed for the paired run order and bootstrap")
    parser.add_argument("--allow-unverified-builds", action="store_true",
                        help="run comparisons whose local builds cannot be traced to a"
                        " clean source tree, recording them as unverified")
    parser.add_argument("--soak-seconds", type=float, default=120,
                        help="length of each soak (default: 120, a smoke run)")
    parser.add_argument("--soak-interval", type=float, default=20,
                        help="seconds between soak checkpoints (default: 20)")
    parser.add_argument("--soak-actions", type=lambda value: value.split(","),
                        default=None,
                        help="comma-separated soak actions to use (default: all)")
    parser.add_argument("--soak-seed", type=int, default=1,
                        help="seed for the soaks' actions (default: 1)")
    parser.add_argument("--perf-trace", action="store_true",
                        help="trace native Wayland's stages for each typing sample")
    parser.add_argument("--perf-presentation", action="store_true",
                        help="request presentation feedback for native Wayland commits")
    parser.add_argument("--perf-idle-seconds", type=float, default=0,
                        help="also measure this many seconds of idle CPU per run")
    args = parser.parse_args()
    if args.list:
        print("\n".join(CASES))
        return 0
    for name in args.cases:
        if name not in CASES:
            parser.error(f"unknown scenario {name!r}")
    if args.timeout <= 0:
        parser.error("--timeout must be positive")
    if any(case in PERF_CASES for case in args.cases):
        if args.perf_pairs < 2:
            parser.error("--perf-pairs must be at least 2")
        if not math.isfinite(args.perf_idle_seconds) or args.perf_idle_seconds < 0:
            parser.error("--perf-idle-seconds must be finite and nonnegative")
        if args.perf_renderer == "gles2" and not args.render_node:
            parser.error("GLES2 performance runs require --render-node")
        if args.render_node and not args.render_node.is_char_device():
            parser.error("--render-node must name a DRM character device")
    args.build_dir = args.build_dir.resolve()
    args.emacs = args.build_dir / "src/emacs"
    args.emacsclient = args.build_dir / "lib-src/emacsclient"
    args.x_emacs = None
    args.mockcomp = args.mockcomp.resolve()
    binaries = [args.emacs]
    if any(case in args.cases for case in (PARITY_CASE, CHILD_PARITY_CASE, *PERF_CASES,
                                          INPUT_PARITY, IMAGE_PARITY)):
        if not args.pgtk_build_dir:
            parser.error("comparisons require --pgtk-build-dir")
        args.pgtk_emacs = args.pgtk_build_dir.resolve() / "src/emacs"
        binaries.append(args.pgtk_emacs)
    if args.x_build_dir and any(case in PERF_CASES for case in args.cases):
        args.x_emacs = args.x_build_dir.resolve() / "src/emacs"
        binaries.append(args.x_emacs)
    real_captures = (PARITY_CASE, *SWAY_SCALES, *OBSERVER_CASES,
                     *CHILD_SWAY_SCALES, TYPING_PERF, IMAGE_PARITY, *IMAGE_SCALES)
    if any(case in real_captures for case in args.cases) and not args.grim:
        parser.error("pixel capture requires grim")
    real_compositor = (*real_captures, *PERF_CASES, INPUT_PARITY, SOAK_SWAY,
                       INSTRUCTIONS_CASE)
    if (any(case in ("damage-repair-rgb", "subpixel-redraw")
            for case in args.cases)
            or any(case in real_compositor for case in args.cases)):
        if not args.fonts:
            parser.error("selected scenarios require DejaVu fonts")
        if not (args.fonts / "DejaVuSansMono.ttf").is_file():
            parser.error(f"missing DejaVuSansMono.ttf in {args.fonts}")
    if any(case not in real_compositor for case in (args.cases or M0_CASES)):
        binaries.extend((args.emacsclient, args.mockcomp))
    for binary in binaries:
        if not os.access(binary, os.X_OK):
            parser.error(f"executable not found: {binary}")

    args.output.mkdir(parents=True, exist_ok=True)
    artifacts = Path(tempfile.mkdtemp(prefix="m0-", dir=args.output.resolve()))
    commit = os.environ.get("EMACS_WAYLAND_TEST_COMMIT")
    if not commit:
        commit = subprocess.run(["git", "-C", SOURCE, "rev-parse", "HEAD"],
                                capture_output=True, text=True).stdout.strip()
    report = {"commit": commit, "date": datetime.now(timezone.utc).isoformat(),
              "platform": platform.platform(), "python": platform.python_version(),
              "build_dir": str(args.build_dir), "scenarios": []}
    if hasattr(args, "pgtk_emacs"):
        report["pgtk_build_dir"] = str(args.pgtk_build_dir.resolve())
    if args.x_emacs:
        report["x_build_dir"] = str(args.x_build_dir.resolve())
    print(f"Logs and results: {artifacts}", flush=True)
    for index, name in enumerate(args.cases or M0_CASES, 1):
        start = time.monotonic()
        path = artifacts / f"{index:02}-{name}"
        path.mkdir()
        result = {"name": name, "unit": "s", "artifacts": path.name}
        try:
            with ExitStack() as stack:
                scenario = Scenario(args, path, name, stack)
                result["data"] = scenario.data
                scenario.run()
            # A sanitizer finding fails the scenario even if Emacs went on.
            for log in sorted(path.glob("*.log")):
                text = log.read_text(errors="replace")
                report_line = next((line.strip() for line in text.splitlines()
                                    if SANITIZER_REPORT.search(line)
                                    and not LEAK_LINE.search(line)), None)
                require(not report_line, f"{log.name}: {report_line}")
                if owned := wayland_leaks(text):
                    raise Failure(f"{log.name}: the backend leaked {owned[0]}")
            result["passed"] = True
            print(f"PASS {name}", flush=True)
        except (Failure, OSError, ValueError, subprocess.TimeoutExpired) as exc:
            result.update(passed=False, error=str(exc))
            print(f"FAIL {name}: {exc} ({path})", file=sys.stderr, flush=True)
        result["seconds"] = time.monotonic() - start
        report["scenarios"].append(result)
        (artifacts / "results.json").write_text(json.dumps(report, indent=2) + "\n",
                                                encoding="utf-8")
    return 0 if all(case["passed"] for case in report["scenarios"]) else 1


if __name__ == "__main__":
    sys.exit(main())
