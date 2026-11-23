# Native Wayland tests

From the repository root, run the M0 gate against a configured Wayland
build. The development shell supplies libwayland, Python, and sway.

```sh
nix develop -c make -C test/wayland check BUILD_DIR="$PWD/../build-wayland"
```

The gate runs mockcomp's self-tests, then launches real GUI Emacs
processes for the transport tests. `startup-background` stays in Lisp
in its init file, as a long one does, until the first configure is
answered with the frame's background, then checks that the first
image replaces it. The gate also checks normal startup, this
background check and daemon frame creation on headless sway with
Xwayland disabled. Mockcomp sends events that make Emacs flush its
requests during Lisp, and sway does not, so only the sway check
catches an initial commit left unsent. Each
scenario has a private runtime directory and a wall-clock deadline.
The Lisp tests report checkpoints over a local socket so the runner
can inject faults while Emacs is inside a process wait or redisplay.

Run named scenarios after building mockcomp, or list the available
scenarios with `--list`.

```sh
nix develop -c python3 test/wayland/run.py --build-dir ../build-wayland write-only epipe
```

The runner prints the directory containing `results.json`, Emacs's
Wayland trace and stderr, and the compositor logs. It retains these
files on failure, including timeouts. Use `--output` to change the
default parent directory, `test/wayland/artifacts/`. The `epipe`
scenario deliberately injects a protocol error and checks that Emacs
records the interface, object ID and code.

The first M1 rendering checks run separately.

```sh
nix develop -c make -C test/wayland check-rendering BUILD_DIR="$PWD/../build-wayland"
```

`pixel-baseline` and `glyph-pixels` compare the PNGs written by Emacs
and mockcomp for the same content commits at 100% scale. The glyph
case changes the text while keeping its width and face fixed. The
pinned Cairo build encodes both images. The resulting `canvas` and
`submitted` directories are kept with the report.

`interrupted-redraw` throws after one glyph draw, then releases the
frame callback and sends a configure. It checks that the previous
buffer stays mapped until a full redraw acknowledges the configure.
`commit-loss` drops the connection at a content commit. The M0
`redisplay-loss` case checks loss during a glyph draw via the
ordinary nonblocking service routine.

`pool-lifetime` starts by sending a configure that keeps the size
while mockcomp holds all three buffers. It must be acked at once, by a
commit that attaches nothing.

While the compositor holds every buffer, Emacs has none to draw into.
It discards the drawing and redraws the frame in full at the first
redisplay after a release. `damage-repair` ends that way: a redraw
with all three buffers held must count as discarded and copy and
commit nothing, and the one after a release must match the frame as
it was. `damage-repair-rgb` repeats the pixel check with RGB subpixel
antialiasing enabled. `subpixel-redraw` swaps words beside unchanged
text under the same antialiasing, with a face for each word and a
header line that changes its word and face: twelve rounds must leave
the same frame as two, and the frame must stay within 32 of each color
channel of a full redraw. `pool-lifetime` checks the same recovery after a pool
allocation fails and after a resize waits for the pool budget.
`pool-budget-recovery` checks that a waiting frame resumes when
another frame releases a charged pool. The `:discards` counter of
`wayland--metrics` counts these events. The soak reports it at each
checkpoint.

`pool-lifetime`, `pool-budget` and `damage-repair` also check their
counters against `budgets.json`: repaired bytes, full repairs, scroll
bytes and peak pool bytes. Repaired bytes and full repairs count the
copies that bring a buffer up to date before drawing. Buffer age
describes the slot selected for drawing, including when no copy is
needed. The fixtures are deterministic, so each
budget is the measured value plus 20%. After a deliberate change,
rerun the three cases with `--record-budgets` and review the diff of
`budgets.json` like any other change. Release latency depends on
timing, so it is recorded in `results.json` as a diagnostic only.

## PGTK pixel comparison at 100%

Configure a second out-of-tree build of the same source with
`--with-pgtk --without-native-compilation`. Build both configurations,
including their portable dumps, inside the pinned Nix shell.

```sh
nix develop -c make -C test/wayland check-parity \
  BUILD_DIR="$PWD/../build-wayland" PGTK_BUILD_DIR="$PWD/../build-pgtk"
```

The runner launches each build fullscreen in the same private headless
sway session at scale 1. It checks the font used on the cursor glyph
against the pinned DejaVu Sans Mono file. `grim` captures the window
including its fringes. A color marker identifies the presented scene
after each cursor change. The check compares decoded RGB pixels for the
horizontal bar, hollow box and erased cursor scenes. Each scene also
contains clipped text, a custom fringe bitmap and an inline image. Both
builds then display a 96×96 canvas and present a new color after
`canvas-refresh` and an ordinary redisplay. The check compares both
canvas captures pixel for pixel and checks that the old color is gone.
The report keeps both sets of PPM captures and the client traces.

`sway-image-parity` shows one image of each format:

- PNG and SVG files from `etc/images`;
- an inline SVG;
- JPEG, TIFF, WebP and a three-frame animated GIF from `images/`;
- inline XPM, PBM and XBM data, and a canvas.

The scene checks that each displays as an image rather than its text.
Its pixels must match PGTK's, and the comparison repeats as
`image-show-frame` steps the GIF through frames 1 and 2. Each GIF
frame is a block of its own color, so each capture must show the
current frame and not the previous one.
The final capture checks an in-place canvas refresh without an edit to
the buffer.

`check-scale` also runs the scene on Wayland at 125%, 150% and 200%
(`sway-image-*`). These crop the window from a capture of the whole
output and compare the initial scene with the reviewed
`references/images-<scale>.png`. The later phases also compare every
pixel. Their smaller reference PNGs store the RGB differences from the
initial scene using modular subtraction. A failed comparison saves the
full expected image beside the capture. For example, `gif-1.png` is
paired with `gif-1-expected.png`. The runner continues collecting
captures for the other phases.

To update the references, review the four captures from one run at the
same scale. If you replace `images-<scale>.png` with the reviewed initial
capture, regenerate **all three** difference files against that new
initial image. Run this from the repository root, with the scale and
the directory containing that run's phase PNGs.

```sh
scale=150
captures=/path/to/run/01-sway-image-150
nix develop -c python3 - "$scale" "$captures" <<'PY'
import sys
from pathlib import Path

from PIL import Image, ImageChops

scale, captures = sys.argv[1:]
captures = Path(captures)
references = Path("test/wayland/references")
with Image.open(references / f"images-{scale}.png") as image:
    initial = image.convert("RGB")
for phase in ("gif-1", "gif-2", "canvas-refresh"):
    with Image.open(captures / f"{phase}.png") as image:
        capture = image.convert("RGB")
    if capture.size != initial.size:
        raise SystemExit(f"{phase} capture size differs from the initial image")
    ImageChops.subtract_modulo(capture, initial).save(
        references / f"images-{scale}-{phase}.png", optimize=True)
PY
```

## Input checks

```sh
nix develop -c make -C test/wayland check-input BUILD_DIR="$PWD/../build-wayland"
```

Mockcomp types keys by evdev code and computes the modifiers with its
own xkb state, as a compositor does. `keyboard` checks characters
with each modifier, function and keypad keys, a dead key on `us:intl`,
the German layout, and focus moving away and back. `key-repeat`
holds a key at 50 repeats a second, checks that release, a zero rate
and focus loss stop it, and records the counts. `quit` sends C-g
during a busy Lisp loop and during an `accept-process-output` wait;
each must quit within 2 seconds, which polling alone would not
guarantee.
`pointer` enters the frame and checks where clicks land at scales 1
and 2, double clicks, shifted clicks, the right and back buttons, a
drag, the text cursor over text and the hand cursor over the mode
line's buffer name, and a tool bar button. During the drag,
`mouse-absolute-pixel-position`, which dragging window edges uses,
must follow the pointer, also outside the frame. Mockcomp's manual clock
spaces the groups of clicks beyond `double-click-time`.
`pointer-theme` runs mockcomp with `--no-cursor-shape`, so cursors
come from the cursor theme.
`scroll`, `scroll-v5` and `scroll-v4` run with mockcomp's seat at
versions 8, 5 and 4: wheel notches each way and sideways, a shifted
notch, high-resolution half steps (version 8), and finger scrolling
with pixel deltas, `touch-end`, coalescing into lines and doubled
deltas at scale 2 (version 5 and later). From version 5 they also
check the `device-class` of each scroll event: `mouse` for a wheel,
`touchpad` for a finger and `trackpoint` for a continuous source.
`input-method` uses mockcomp's text-input-v3 role: enabling on
focus, the cursor rectangle at point's glyph, preedit shown in the
overlay, commits as characters, the cursor rectangle held back after
a done with the wrong serial, a commit in flight across a focus change
to a second frame, and `wayland-use-input-method` set to nil.

The socket raises SIGIO, so the input hook can run at any safe point.
`quiet-ping`, `write-only`, `epipe` and `redisplay-loss` test the paths
that must work without the hook, so they turn SIGIO off first with
`wayland--debug-sigio`.

## Capture observer

```sh
nix develop -c make -C test/wayland check-observer BUILD_DIR="$PWD/../build-wayland"
```

`observer/observer` measures input to first captured output commit on
headless sway: it arms a wlr-screencopy capture of a region, presses a
key through virtual-keyboard, and reports when the region first
changes. wlroots stamps captures with `CLOCK_MONOTONIC`, the
observer's own clock. `observer/refclient` flips a pre-drawn,
physical-size buffer on each key. `observer-reference` runs the
observer three times against it at 150% and requires the medians to
agree within 2 ms; `observer-emacs` records the latency of typing into
Emacs. Each run's samples are kept as `observer-N.jsonl`. With
`--watch MS` the observer instead reports each changed capture of its
region for that long. `observer/vkbd` and `observer/vptr` are
long-lived virtual keyboard and pointer helpers driven by lines on
stdin; `vptr` keeps its own schedule for timed finger strokes.

## Paired performance pilots

```sh
nix develop -c make -C test/wayland check-perf \
  BUILD_DIR="$PWD/../build-wayland" PGTK_BUILD_DIR="$PWD/../build-pgtk" \
  X_BUILD_DIR="$PWD/../build-x11" \
  PERF_ARGS="--perf-pairs 12 --perf-renderer gles2 --render-node /dev/dri/renderD128"
```

Build from the same revision with matching shared options. Configure
the X reference with `--with-x-toolkit=no --with-cairo`. The flake's
`nix run .#check-wayland-perf` stages all three builds.
The runner refuses a local build unless:

- they come from one source tree, with no uncommitted changes outside
  `admin/notes` and `test/wayland`;
- each was dumped at that tree's HEAD;
- each executable is newer than the tracked files under `src` and
  `lib`;
- each `emacs.pdmp` is also newer than the tracked files under `lisp`.

Reverting a file gives it a new time, so a build made before the revert
must be rebuilt. Binaries that resolve into the Nix store, including
the flake's staged symlinks, are pinned by their store path's revision
instead. `--allow-unverified-builds` runs anyway and records
`builds_verified: false`.

Each scenario starts headless sway at 150% with a seat keyboard and
launches a fresh Emacs for each measurement. The seeded order rotates by
one backend per group. Run a multiple of three groups for equal first
positions with the X reference. The X runs use an isolated sway with
Xwayland enabled and record Xwayland's version and arguments. CPU and
memory measurements for X include both Emacs and Xwayland.

The JSON includes build, renderer and host metadata, including the power
supply state before and after each scenario, raw samples,
per-run results and bootstrap intervals for ratios to PGTK and X.
On failure it retains completed runs and identifies the active run
and its log. `PERF_CASES` selects among:

- `typing-perf` calibrates the observer, then types into a fontified
  copy of `src/xdisp.c` with line numbers enabled. It checks every
  captured change and the resulting text. Each key waits a
  seeded random part of the output's refresh period, the same for all
  runs of a pair, so that samples do not all start just after an output
  commit. `--perf-trace` also records native Wayland's stages with
  `wayland--trace` and splits each sample at them; leave it off for
  comparisons. Each run samples
  PSS, RSS and USS after the frame settles and records live and retired
  Wayland pool bytes. Add `--perf-idle-seconds 60` to measure process
  CPU time before typing, with the cursor off and no screencopy, and
  again after typing with a cursor blinking without limit. The native
  Wayland run checks zero content commits in the first interval.
- `scroll-perf` injects 10 seconds of 120 Hz finger scrolling through a
  virtual pointer, with `pixel-scroll-precision-mode` on, and measures
  Emacs's CPU time until input is quiet. PGTK scrolls a fixed 10
  pixels per compositor pixel of finger travel and native Wayland
  scrolls as X does, by the window's height, so each gets the travel
  that covers the same distance. The runner checks that the builds
  report the same scroll settings and scrolled the same logical
  distance.
- `scroll-cadence` repeats the scroll while the observer watches a band
  of the text, and records the interval between changed captures and
  intervals longer than 1.5 refresh periods. With `--perf-trace` it
  describes each long interval from the native Wayland trace. CPU time
  for this workload includes the observer's load.
- `startup-perf` launches `emacs -Q` and reports the first changed
  capture as `first_window_ms`. `first_content_ms` is the first capture
  of the marker background Emacs draws, after anything the X server,
  GTK or the native background buffer paints first. Emacs sets the
  marker from the test's first timer, after the first configure was
  answered. `startup-init-perf` uses an isolated init directory
  and loads a fixed set of built-in libraries. Both also report Elisp
  init time, and keep redisplaying while they wait, as the idle command
  loop would. With `--perf-trace` they keep the native Wayland trace.
- `redraw-perf` forces 30 full redisplays at 3840×2160 and 150%,
  paced so the observer can see each redraw. With `--perf-trace` it
  and `canvas-perf` keep the native Wayland trace and report the
  median time between commit markers as `present_stages`. The
  `copies` fields give the count and total elapsed time of restoring
  a target before drawing. They overlap the commit intervals. The
  trace also counts and times restores followed by a full clear
  before the next commit.
- `resize-perf` asks sway for 200 sizes at 60 Hz and records request
  timestamps, client sizes and changed captures. Emacs runs its
  normal command loop during the requests. The runner compares
  captured updates per second and the number of sizes Emacs laid
  out. With `--perf-trace` it records native Wayland configure,
  acknowledgement and commit timestamps and whether the commit
  attached a buffer.
- `scale-perf` changes the output from 150% to 200%. It records the
  first visible change, the first verified frame geometry, and when a
  repaint with that geometry reaches the capture observer.
- `clipboard-perf` measures yank latency and serving throughput for
  1 KiB, 1 MiB and 64 MiB of text in each direction, and compares the
  time of each transfer. Emacs times its own yanks, since its wait for
  the runner's command polls. Every transfer is checked against a
  SHA-256 hash.
- `canvas-perf` refreshes a 512×512 canvas 300 times at 150% and
  records CPU time and changed captures per second.
- `image-scroll-perf` times scrolling through alternating large PNG
  and SVG images.
- `frames-perf` maps ten frames on two outputs and records CPU, PSS,
  RSS and USS.

Omit `PERF_ARGS` for three pixman groups at 150%. The scene
environment sets `GDK_SCALE=1`, so at 150% PGTK draws at 1× like X
under Xwayland, and only native Wayland draws every physical pixel.
`--perf-scale 100` runs the workloads on a 1× output, where all three
draw the same pixels. Add `--perf-presentation` to
count native Wayland's presented and discarded content commits in the
typing, scroll, redraw and canvas workloads. The runner checks that
every request received feedback. These runs are diagnostics until the
dedicated timing host qualifies.

## Desktop settings

```sh
nix develop -c make -C test/wayland check-m4 BUILD_DIR="$PWD/../build-wayland"
```

Every scenario runs without a session bus, so the desktop's fonts and
text scaling cannot reach either backend. Every scenario also uses a
fontconfig file limited to the pinned DejaVu fonts, in which
`Monospace` is DejaVu Sans Mono.
`desktop-settings` starts a private bus with no service directories,
so nothing is activated on it.
The Emacs under test serves the settings portal's `ReadAll` itself and
checks the system font, a text scaling change, that a monospace change
applies only with `font-use-system-font`, and a new frame's font.

`check-m4` also exercises these mock compositor scenarios.

- `frame-states` checks withheld frame callbacks, suspend and resume,
  a burst of resize configures, and title and iconify flushes
  while Emacs stays in Lisp. Fullscreen, maximized and tiled
  configures must keep their size through font changes and
  `set-frame-size`, and set the `fullscreen` frame parameter.
- `seat-changes` removes and restores keyboard and pointer
  capabilities. It checks keyboard focus, release of the devices,
  the keymap, key repeat and the replacement pointer, and that
  `display-mouse-p` follows the seat's pointer.
- `input-queue` holds Emacs away from the display socket while a
  burst of keys and a clipboard request arrive, so that one dispatch
  overflows the keyboard FIFO. It verifies that the clipboard still
  sends its payload and ordinary keyboard input resumes.
- `minimal-protocols` starts with the minimum versions of the needed
  globals and without the optional extensions.

## Sanitizers and soak

Build with AddressSanitizer or UndefinedBehaviorSanitizer as
`etc/DEBUG` describes (the two cannot be combined), and run any target
against that build. The runner sets `ASAN_OPTIONS` and `UBSAN_OPTIONS`
to `etc/DEBUG`'s values unless they are already set, and fails a
scenario whose Emacs log contains a sanitizer report, even if Emacs
carried on. For a checking build, add `--enable-checking=yes,glyphs`
and `--enable-check-lisp-object-type` to the UBSan options.

Leak detection is off by default, as `etc/DEBUG` advises. To run with
LeakSanitizer:

```sh
ASAN_OPTIONS=detect_leaks=1,detect_stack_use_after_return=0 \
LSAN_OPTIONS=exitcode=0 \
nix develop -c make -k -C test/wayland check-all BUILD_DIR="$PWD/../build-asan"
```

At exit, LeakSanitizer also reports hash tables restored from the
portable dump, whose mapping it does not scan, and objects of the font
drivers that the X builds share. So a leak fails a scenario only when
the backend allocated it: when the first Emacs frame above the
allocators is in `src/wayland*.c`.

Valgrind memcheck runs the Wayland Emacs under `--wrapper`, with
deadlines scaled by `--timeout-scale`. `valgrind.supp` suppresses the
garbage collector's conservative stack scan and nothing reached through
the backend. Its entries match the `garbage_collect` frame, so keep
`--num-callers` above the deepest marking chain:

```sh
nix develop -c python3 test/wayland/run.py --build-dir ../build-wayland \
  --timeout-scale 15 --wrapper "valgrind --error-exitcode=99 \
    --track-origins=yes --num-callers=200 \
    --suppressions=$PWD/test/wayland/valgrind.supp" \
  startup child-frames selection
```

```sh
nix develop -c make -C test/wayland soak-smoke BUILD_DIR="$PWD/../build-wayland"
nix develop -c make -C test/wayland soak BUILD_DIR="$PWD/../build-wayland"
```

`soak` runs for `SOAK_SECONDS` (two hours by default) and `soak-smoke`
for two minutes. `soak-storm` drives Emacs on mockcomp with seeded
storms of typing, wheel and finger scrolling, configure floods, scale
changes, held buffers, withheld frame callbacks, output hotplug and
child frames, and ends by disconnecting. `soak-sway` types, scrolls,
resizes, changes the output scale, shows and hides a child frame and
copies and pastes through `wl-copy` and `wl-paste` on headless sway.
Each saves its seed and the actions it took as `soak-trace.jsonl`;
`--soak-seed` and `--soak-actions` (in `SOAK_ARGS`) replay a run or
narrow it to some actions. At checkpoints every `--soak-interval`
seconds the runner resets the faults, waits for Emacs to settle and
records its frames, buffers, processes, open descriptors and Wayland
pools, which must not drift after warm-up, and its PSS and live Lisp
bytes, whose median over the late half of the run must stay within a
margin of the early half's. On mockcomp it also records Emacs's live
Wayland objects by interface: for each interface, the late half's
largest count must not exceed the early half's.

## Input parity

```sh
nix develop -c make -C test/wayland check-input-parity \
  BUILD_DIR="$PWD/../build-wayland" PGTK_BUILD_DIR="$PWD/../build-pgtk"
```

On headless sway, `wtype`, `vkbd` and `vptr` send the same keys, dead-key
compose, clicks, double clicks, drags, a Shift-click, wheel notches,
the side and back buttons and finger strokes to each backend. The
runner compares the Emacs event streams, with the column and row of
each mouse event, and saves both. Intentional differences are
normalized first, where this backend follows X: GTK names BTN_BACK
`mouse-11`, as the X server does, where this backend makes it `mouse-8`
like BTN_SIDE, and PGTK names scrolling right `wheel-left`. Each finger
stroke is compared by direction and terminator, since each backend
carries its own remainder between strokes. The runner checks that
each stroke sends an event per 10 compositor pixels and that the pixel
deltas `pixel-scroll-precision-mode` reads follow X's rule, a unit's
travel moving the window height to the 2/3 power.

## A real input method

```sh
nix develop -c make -C test/wayland check-fcitx5 BUILD_DIR="$PWD/../build-wayland"
```

`sway-fcitx5` runs fcitx5 with its Hangul engine on headless sway and
types g k s w k, then Space, through `observer/vkbd`, a virtual
keyboard that lives for the whole scenario so that it exists before
fcitx5 grabs the keyboard. It checks that 한 is committed, that 자 is
shown as preedit, and that Space commits 자. The pinned shell provides
fcitx5 and fcitx5-hangul; fcitx5 runs without D-Bus.
`make -C test/wayland check-m2` runs every M2 check.

## Child frames, clipboard and Lisp integration

```sh
nix develop -c make -C test/wayland check-m3 \
  BUILD_DIR="$PWD/../build-wayland" PGTK_BUILD_DIR="$PWD/../build-pgtk"
```

The child scenarios check the wire order of the position and first
buffer on map and remap with an idle parent. They also cover independent
content commits, sibling stacking, parent-state commits with a pending
resize, inherited visibility, nested input, reparenting, scale changes
and the optional scaling and subcompositor protocols. `child-pointer`
and `child-drag` check `mouse-absolute-pixel-position` over a child:
in its parent's coordinates, and after the child moves under a held
button, still relative to where it was at the press, as compositors
report the pointer then.

On sway, a popup is shown, moved, resized and hidden while the tree
keeps one toplevel. The runner checks exact popup bounds at 100%, 125%
and 150%. The 100% case compares the capture with PGTK, using its Cairo
renderer in the same sway session. The test keeps processing GTK
events while the runner captures. The report retains both sets of
compositor captures.

The selection fixture checks focus and source replacement, a read
deadline, C-g and a slow reader that drains the selection pipe for
longer than the idle timeout. `sway-clipboard-pipes` checks 1 KiB,
1 MiB and 64 MiB transfers with `wl-copy` and `wl-paste`, including
PRIMARY and reader backpressure. `popup-menu` covers labels, selection and
cancellation. `activation` exercises the token from an emacsclient
request, a token requested by Emacs and pending tokens across title
changes and role replacement.

## Instructions per redisplay

```sh
nix develop -c make -C test/wayland check-instructions \
  BUILD_DIR="$PWD/../build-wayland"
```

`redisplay-instructions` runs Emacs under Callgrind on headless sway
and counts the instructions executed inside `redisplay` for three
fixed workloads in `instructions.el`: forty page scrolls and sixty
line scrolls through fontified C text with line numbers, and sixty
typed characters. Font lock is off in the copy and no garbage
collection runs, so the count covers layout, drawing and the commit.
Layout and drawing repeat to within 0.2%. Restoring the buffer from
the one on screen copies more when sway has not yet released the
latest buffer, which moves the typing count by about 1%.

The case fails when a workload exceeds its limit in `budgets.json`,
which is the recorded count plus 1%, or 3% for typing. Counts depend on the compiler
flags and on the CPU, which decides the routines that pixman and
glibc use, so the budget names the configure flags, `CFLAGS` and CPU
model it was recorded with. On any other build or machine the case
reports its counts without comparing them. Record a budget there
with `--record-budgets`, and again after a rebase or a deliberate
change, and review the diff like the other budgets. To compare two
revisions, record on one and run the other.

## Upstream changes to copied PGTK code

```sh
make -C test/wayland check-drift
```

The backend's glyph, relief, underline and image drawing began as
copies of functions in `src/pgtkterm.c`. `pgtk-sync.json` records a
hash of each original, ignoring comments and whitespace, as of the
last time its copy was brought up to date. After a rebase, the check
lists the originals that changed upstream. Port each change, using
`pgtk-drift.py --diff FUNCTION` to compare an original with its copy,
then run `pgtk-drift.py --record`. Record again after adding or
removing a copy.
