"""Diff two stutter_snapshot.sh captures and print only what actually changed.

The bug is intermittent and self-clearing, so no single snapshot can identify it: every value in a
healthy capture looks reasonable, and every value in a broken one looks reasonable too until you
have the other to compare against. Whatever differs between "bad" and "good" is the candidate set,
and everything identical in both is eliminated -- including, usefully, the things this project
currently suspects.

  sh tools/stutter_snapshot.sh bad     (while it drags, BEFORE toggling the screen)
  sh tools/stutter_snapshot.sh good    (after the toggle fixes it)
  python tools/stutter_diff.py

Lines that are expected to differ between any two captures -- clocks, temperatures, counters --
are listed as NOISE rather than hidden, because "the CPU was at a different frequency" is exactly
the observation that would matter if the frequencies were the wrong ones. The report separates
them from lines that should have been identical and were not; read those first.
"""
import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# Keys whose value legitimately changes second to second. Different here is not evidence by
# itself, so they are reported apart -- never dropped, because a 500 MHz gap in `cur` is the
# whole question when the complaint is that the phone is slow.
NOISE = re.compile(
    r"^(zone |gpu_clock$|gpu_busy$|gpu_tmu$|sf_pid$|sf_starttime|online $|"
    # Live frequencies move constantly; the WINDOW around them (min/max/gov) does not, and those
    # are matched by STABLE_PREFIX below so they can never fall in here.
    r"policy\d+\.cur$|.*\.cur_freq$|"
    # The engine advances these every window by design. A run that did not advance them would be
    # the finding, but that is a different report.
    r"(samples|windows|contexts|policies|confidence|reason|regime|budget|ceiling)$|"
    r"journal:$|.*_time$|.*Timestamp$)")
# Keys that must not differ between two captures minutes apart on one boot. A change here is the
# finding, not context.
STABLE_PREFIX = ("prop ", "sf_backend", "pelt", "cool ")
# Per-policy fields that must not move between two captures on one boot. `cur` is excluded on
# purpose -- it is the live frequency and belongs to NOISE.
STABLE_SUFFIX = (".min", ".max", ".gov", ".hwmax")


def pull(device, label):
    prefix = ["adb"] + (["-s", device] if device else [])
    remote = "/data/local/tmp/snap_{}.txt".format(label)
    stage = "/data/local/tmp/pulled_snap_{}.txt".format(label)
    subprocess.run(prefix + ["shell", "su 0 sh -c 'cp {0} {1} && chmod 644 {1}'".format(remote, stage)],
                   capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / (label + ".txt")
        got = subprocess.run(prefix + ["pull", stage, str(local)], capture_output=True, text=True)
        return local.read_text(encoding="utf-8", errors="replace") if got.returncode == 0 and local.exists() else ""


def sections(text):
    """Split a capture into its '## name' sections, keeping line order inside each."""
    out, name = {}, "head"
    for line in text.splitlines():
        if line.startswith("## "):
            name = line[3:].split(" -- ")[0].strip()
            out.setdefault(name, [])
            continue
        out.setdefault(name, []).append(line.rstrip())
    return out


def keyed(lines):
    """Map a section's lines to key -> value.

    Two shapes have to be handled, because dumpsys does not use the same one as sysfs: `k = v`
    from this project's own probes, and `Total frames rendered: 3` from gfxinfo. Splitting only on
    '=' left the whole gfxinfo line as a key, so two captures with different values produced two
    different KEYS and every one of them showed up as "present in one capture only".

    A '--' line starts a sub-block and becomes part of the key. Without that, the lifetime and the
    windowed gfxinfo blocks -- identical key names, deliberately different meanings -- overwrite
    each other and the whole comparison silently collapses to whichever came last.
    """
    out, prefix = {}, ""
    for line in lines:
        if not line.strip():
            continue
        if line.startswith("--"):
            prefix = line.strip("- ").strip() + " / "
            continue
        key, sep, value = line.partition("=")
        if not sep:
            key, sep, value = line.partition(":")
        out[prefix + (key.strip() if sep else line)] = value.strip() if sep else ""
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bad", type=Path, help="a snapshot already on disk")
    parser.add_argument("--good", type=Path)
    parser.add_argument("--device", default="")
    args = parser.parse_args()

    bad = args.bad.read_text(encoding="utf-8", errors="replace") if args.bad else pull(args.device, "bad")
    good = args.good.read_text(encoding="utf-8", errors="replace") if args.good else pull(args.device, "good")
    if not bad.strip() or not good.strip():
        print("need both snapshots on the device:")
        print("  sh tools/stutter_snapshot.sh bad     (while it drags, before the screen toggle)")
        print("  sh tools/stutter_snapshot.sh good    (after the toggle fixes it)")
        return 1

    b, g = sections(bad), sections(good)
    findings, noise, only = [], [], []
    for name in [n for n in b if n in g] + [n for n in b if n not in g]:
        kb, kg = keyed(b.get(name, [])), keyed(g.get(name, []))
        for key in kb:
            if key not in kg:
                only.append((name, key, kb[key], "<absent>"))
                continue
            if kb[key] == kg[key]:
                continue
            row = (name, key, kb[key], kg[key])
            stable = key.startswith(STABLE_PREFIX) or key.endswith(STABLE_SUFFIX)
            (findings if stable or not NOISE.match(key) else noise).append(row)
        for key in kg:
            if key not in kb:
                only.append((name, key, "<absent>", kg[key]))

    def show(title, rows, note):
        print(title)
        if not rows:
            print("  (none)")
        else:
            for name, key, x, y in rows[:60]:
                print("  [{}] {}".format(name, key))
                print("      bad : {}".format(x if x else "(empty)"))
                print("      good: {}".format(y if y else "(empty)"))
            if len(rows) > 60:
                print("  ... {} more".format(len(rows) - 60))
        print("  {}".format(note))
        print("")

    show("CHANGED, and should not have been -- read these first",
         findings,
         "A render prop or sf_backend differing means the screen toggle changed the graphics stack.\n"
         "  A policy ceiling differing means something was capping the CPU and then released it.\n"
         "  A cooling_device state differing names the thing that was doing the capping.")
    show("PRESENT IN ONE CAPTURE ONLY", only,
         "Usually a dumpsys section that was empty on one side; check it is not the interesting one.")
    show("CHANGED, but changes on its own anyway", noise,
         "Context, not evidence -- unless the magnitude is the complaint. A big cluster sitting\n"
         "  600 MHz lower in the bad capture is the symptom, so read `cur` here even though it moves.")
    if not findings:
        print("Nothing stable differed. That does NOT mean the two captures were the same machine:")
        print("it means the difference is not in anything this snapshot records. Widen it -- the")
        print("next places to look are the display mode list and the composer HAL, neither of which")
        print("is fully captured here.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
