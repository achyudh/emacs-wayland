### Native Wayland M0 checks.  Run inside 'nix develop'.

## This file is part of GNU Emacs, as modified by the native Wayland
## backend fork.  It is free software under
## the GNU General Public License, version 3 or later.

PYTHON ?= python3
BUILD_DIR ?= ../..
PGTK_BUILD_DIR ?= ../../../build-pgtk
X_BUILD_DIR ?=
PERF_ARGS ?=
PERF_CASES ?= typing-perf scroll-perf scroll-cadence image-scroll-perf \
	startup-perf startup-init-perf redraw-perf resize-perf scale-perf \
	clipboard-perf canvas-perf frames-perf

check:
	$(MAKE) -C mockcomp check
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)"

check-rendering:
	$(MAKE) -C mockcomp
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  pixel-baseline glyph-pixels interrupted-redraw commit-loss \
	  pool-lifetime pool-budget pool-budget-recovery damage-repair \
	  damage-repair-rgb subpixel-redraw mid-draw mid-redisplay-write \
	  scale-fractional scale-no-viewporter scale-no-fractional scale-outputs

check-parity:
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  --pgtk-build-dir "$(PGTK_BUILD_DIR)" sway-pixel-parity sway-image-parity

check-scale:
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  sway-scale-125 sway-scale-150 sway-scale-200 sway-scale-odd \
	  sway-image-125 sway-image-150 sway-image-200

check-m1: check-rendering check-parity check-scale

check-observer:
	$(MAKE) -C observer
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" observer-reference observer-emacs

check-fcitx5:
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" sway-fcitx5

check-input:
	$(MAKE) -C mockcomp
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" keyboard key-repeat quit \
	  pointer pointer-theme scroll scroll-v5 scroll-v4 input-method

check-m2: check-input check-observer check-fcitx5

check-perf:
	$(MAKE) -C observer
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  --pgtk-build-dir "$(PGTK_BUILD_DIR)" \
	  $(if $(X_BUILD_DIR),--x-build-dir "$(X_BUILD_DIR)") $(PERF_ARGS) $(PERF_CASES)

check-input-parity:
	$(MAKE) -C observer
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  --pgtk-build-dir "$(PGTK_BUILD_DIR)" sway-input-parity

check-m3:
	$(MAKE) -C mockcomp
	$(MAKE) -C observer
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" child-frames child-hierarchy \
	  child-no-subcompositor child-no-viewporter child-no-fractional \
	  child-mixed-scale child-pointer child-drag selection popup-menu \
	  activation \
	  sway-clipboard-pipes
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  --pgtk-build-dir "$(PGTK_BUILD_DIR)" \
	  sway-child-parity sway-child-125 sway-child-150

check-m4:
	$(MAKE) -C mockcomp
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" \
	  desktop-settings frame-states seat-changes input-queue minimal-protocols

SOAK_SECONDS ?= 7200
SOAK_ARGS ?=

soak:
	$(MAKE) -C mockcomp
	$(MAKE) -C observer
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" --soak-seconds $(SOAK_SECONDS) \
	  $(SOAK_ARGS) soak-storm soak-sway

soak-smoke:
	$(MAKE) soak SOAK_SECONDS=120

check-runner:
	$(PYTHON) runner_tests.py

# After a rebase: the PGTK functions the backend copies, changed upstream.
check-drift:
	$(PYTHON) pgtk-drift.py

# Instructions per redisplay under Callgrind, against budgets.json.
check-instructions:
	$(PYTHON) run.py --build-dir "$(BUILD_DIR)" redisplay-instructions

check-all: check-runner check-drift check check-m1 check-m2 check-m3 check-m4 \
  check-input-parity

.PHONY: check check-rendering check-parity check-scale check-m1 check-input \
  check-observer check-fcitx5 check-m2 check-m3 check-perf \
  check-input-parity check-m4 check-runner check-drift check-instructions \
  soak soak-smoke check-all
