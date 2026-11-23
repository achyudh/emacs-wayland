"""Checks of run.py's own logic that need no compositor.

verify_local_builds: staged Nix symlinks, mixed pairs, and a dump built
before a Lisp revert.  Run by `make check-runner'."""
import os, subprocess, sys, tempfile, time
from pathlib import Path
from types import SimpleNamespace
RUN = Path(__file__).resolve().with_name("run.py")
ns = {"__file__": str(RUN), "__name__": "fixture"}
exec(compile(RUN.read_text(), str(RUN), "exec"), ns)
verify, Failure = ns["verify_local_builds"], ns["Failure"]

root = Path(tempfile.mkdtemp(prefix="emacs-wl-runner-"))
repo = root / "repo"
for name, text in (("src/a.c", "int a;\n"), ("lisp/b.el", ";; b\n")):
    (repo / name).parent.mkdir(parents=True, exist_ok=True)
    (repo / name).write_text(text)
git = ["git", "-C", str(repo), "-c", "user.email=f@x", "-c", "user.name=f"]
subprocess.run(["git", "init", "-q", str(repo)], check=True)
subprocess.run(git + ["add", "."], check=True)
subprocess.run(git + ["commit", "-qm", "base"], check=True)
head = subprocess.run(git + ["rev-parse", "HEAD"], capture_output=True, text=True).stdout.strip()

def build(name, when):
    binary = root / name / "src/emacs"
    binary.parent.mkdir(parents=True, exist_ok=True)
    for path in (binary, binary.with_name("emacs.pdmp")):
        path.write_text("x"); os.utime(path, (when, when))
    return binary

def staged(name):
    link = root / name / "src/emacs"
    link.parent.mkdir(parents=True, exist_ok=True)
    link.symlink_to(f"/nix/store/0000-emacs-{name}-31.0.50+abcdef0/bin/emacs")
    return link

ready = {"source": str(repo), "commit": head}
now = time.time()
results = []
def case(label, builds, readies, expect):
    try:
        got = verify(builds, readies, False)
    except Failure as exc:
        got = f"refused: {exc}"
    ok = (got is True) if expect == "ok" else str(got).startswith("refused")
    results.append(ok)
    print(("PASS" if ok else "FAIL"), label, "->", got)

case("staged Nix symlinks are pinned",
     {"wayland": staged("sw"), "pgtk": staged("sp")}, {"wayland": {}, "pgtk": {}}, "ok")
fresh = build("fresh", now + 10)
case("a fresh local build with a staged store build",
     {"wayland": fresh, "pgtk": staged("sp2")}, {"wayland": ready, "pgtk": {}}, "ok")
# Built with a Lisp change, then the change was reverted: checkout
# rewrites the file with the current time, after the dump.
stale = build("stale", now + 20)
(repo / "lisp/b.el").write_text(";; changed\n")
subprocess.run(git + ["checkout", "--", "lisp/b.el"], check=True)
os.utime(repo / "lisp/b.el", (now + 30, now + 30))
case("a dump built before a reverted Lisp change",
     {"wayland": stale, "pgtk": staged("sp3")}, {"wayland": ready, "pgtk": {}}, "refused")
os.utime(repo / "src/a.c", (now + 40, now + 40))
older = build("older", now + 35)
os.utime(older.with_name("emacs.pdmp"), (now + 50, now + 50))
case("an executable older than a C source",
     {"wayland": older, "pgtk": staged("sp4")}, {"wayland": ready, "pgtk": {}}, "refused")
newer = build("newer", now + 60)
case("both builds fresh", {"wayland": newer, "pgtk": build("newer2", now + 60)},
     {"wayland": ready, "pgtk": ready}, "ok")

# A draw before a full clear can restore an old target, which the
# clear then overwrites.  Keep this time visible in the trace.
trace = [(name, ms * 1000000, 0) for name, ms in (
    ("restore-start", 1), ("restore", 3),
    ("begin", 4), ("clear", 5), ("up-to-date", 7), ("buffer", 8),
    ("repaired", 8), ("commit", 9),
    ("restore-start", 10), ("restore", 11), ("commit", 12),
)]
stages = ns["present_stages"](trace)
ok = (stages["repair_ms"] == 0
      and stages["copies"] == {"restore": {"count": 2, "total_ms": 3}}
      and stages["restores_before_clear"] == 1
      and stages["restore_before_clear_ms"] == 2)
results.append(ok)
print(("PASS" if ok else "FAIL"), "copy stages include restores before a full clear")

# An attachment applies to the next commit of that surface only.
protocol = """[01:02:03.000] -> xdg_wm_base#1.get_xdg_surface(new id xdg_surface#2, wl_surface#3)
[01:02:03.100] xdg_surface#2.configure(10)
[01:02:03.101] -> xdg_surface#2.ack_configure(10)
[01:02:03.102] -> wl_surface#3.attach(wl_buffer#5, 0, 0)
[01:02:03.103] -> wl_surface#3.commit()
[01:02:03.200] xdg_surface#2.configure(11)
[01:02:03.201] -> xdg_surface#2.ack_configure(11)
[01:02:03.202] -> wl_surface#4.attach(wl_buffer#5, 0, 0)
[01:02:03.203] -> wl_surface#4.commit()
[01:02:03.204] -> wl_surface#3.commit()
[01:02:03.300] xdg_surface#2.configure(12)
[01:02:03.301] -> xdg_surface#2.ack_configure(12)
[01:02:03.302] -> wl_surface#3.attach(wl_buffer#5, 0, 0)
[01:02:03.303] -> wl_surface#3.attach(nil, 0, 0)
[01:02:03.304] -> wl_surface#3.commit()
"""
samples = ns["Scenario"].resize_protocol_samples(
    SimpleNamespace(emacs_log=lambda _: protocol), "fixture")
ok = ([sample["buffer_attached"] for sample in samples] == [True, False, False]
      and [round(sample["commit_ms"] - sample["ack_ms"]) for sample in samples]
      == [2, 3, 3])
results.append(ok)
print(("PASS" if ok else "FAIL"), "resize trace matches attachments to commits")
subprocess.run(["rm", "-rf", "--", str(root)])
sys.exit(0 if all(results) else 1)
