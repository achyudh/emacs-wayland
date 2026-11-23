#!/usr/bin/env bash
### Check mockcomp: a correct session records no violation, each
### deliberate violation is recorded, and pings, keys and the keymap
### reach a client.

## This file is part of GNU Emacs, as modified by the native Wayland
## backend fork.  It is free software under
## the GNU General Public License, version 3 or later.

set -uo pipefail
cd "$(dirname "$0")"

dir=$(mktemp -d)
sock="mockcomp-selftest-$$"
control="$dir/control"
pid=

cleanup () {
  [ -n "$pid" ] && kill "$pid" 2>/dev/null
  rm -rf "$dir"
}
trap cleanup EXIT

./mockcomp --socket "$sock" --control "$control" --trace "$dir/trace" \
  > "$dir/out" 2>&1 &
pid=$!
for _ in $(seq 50); do
  grep -q "mockcomp ready" "$dir/out" && break
  sleep 0.1
done
grep -q "mockcomp ready" "$dir/out" || { cat "$dir/out"; exit 1; }

# Send one control command and print its one-line reply.
ctl () {
  python3 -I -c '
import socket, sys
s = socket.socket(socket.AF_UNIX)
s.connect(sys.argv[1])
s.sendall((sys.argv[2] + "\n").encode())
print(s.makefile().readline().strip())' "$control" "$*"
}

# Print FIELD of the JSON status.
status () {
  python3 -I -c '
import json, sys
print(json.loads(sys.argv[1])[sys.argv[2]])' "$(ctl status)" "$1"
}

failures=0
expect () {
  if [ "$2" = "$3" ]; then
    echo "PASS $1"
  else
    echo "FAIL $1: expected '$3', got '$2'"
    failures=$((failures + 1))
  fi
}
expect_violation () {
  case "$(status violations)" in
    *"$2"*) echo "PASS $1" ;;
    *) echo "FAIL $1: no $2 violation"; failures=$((failures + 1)) ;;
  esac
}

export WAYLAND_DISPLAY="$sock"

./selftest good
expect "correct session has no violations" "$(status violation_total)" 0

./selftest damage
expect_violation "damage outside the buffer is caught" damage-outside-buffer

ctl hold-buffers on > /dev/null
./selftest held
expect_violation "reuse of a held buffer is caught" reuse-of-held-buffer
ctl hold-buffers off > /dev/null

./selftest early-attach
expect_violation "attach before configure is caught" attach-before-configure

./selftest role-conflict
expect_violation "conflicting subsurface role is rejected" subsurface-role-conflict
./selftest parent-cycle
expect_violation "cyclic subsurface parent is rejected" subsurface-parent-cycle

./selftest listen 2 > "$dir/listen" &
listener=$!
sleep 0.7
ctl ping > /dev/null
ctl key 30 press > /dev/null
ctl key 30 release > /dev/null
expect "a connected client's surface is mapped" "$(status mapped)" 1
wait "$listener"
expect "pings, keys and keymap reach the client" \
  "$(cat "$dir/listen")" "keys=2 pings=1 keymap=ok"
expect "the pong is counted" "$(status pongs)" 1
expect "violations total" "$(status violation_total)" 5

ctl quit > /dev/null
wait "$pid"
pid=
if [ "$failures" -ne 0 ]; then
  echo "$failures check(s) failed; trace:"
  tail -40 "$dir/trace"
  exit 1
fi
echo "all mockcomp checks passed"
