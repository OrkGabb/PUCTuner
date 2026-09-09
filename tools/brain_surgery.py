"""Targeted repair of a saved brain after the MEANING of a measurement changes.

A schema change is handled by the version tag and zero padding. This is the other case: the
width did not change, but windows already recorded were labelled against something that has
since been proven wrong, and they keep teaching it. That happened on 2026-09-09, when the frame
cadence estimate was found to random-walk between 24 and 120 on a steady 20 fps stream -- the
same p95 of 58.1 ms was stored as deficit 0.131 and as deficit 1.000.

Two operations, both conservative, both reversible from the backup this script writes:

  --drop-replay    Discard the stored replay buffer. This is the one that matters. `rehearse`
                   re-fits the critic directly on each sample's stored cost vector, runs on every
                   idle or frameless window, and does not count as evidence -- roughly 17000
                   rehearsal steps against 256 stored windows in a single day here. Those cost
                   vectors ARE the wrong labels, verbatim, and they outnumber live windows by
                   more than an order of magnitude. The buffer refills from live windows in about
                   half an hour of use.

  --reset-evidence Set the critic's update counter to zero. It feeds only three things: the
                   learning rate (`.02 / (1 + updates/4000)`), the gradient-scale decay, and the
                   reported confidence. At 4005 updates the critic had already halved its own
                   pace, so it would unlearn the contradiction more slowly than it learned it.
                   Nothing gates acting or learning on it, so this costs nothing but an honest
                   confidence reading of zero.

What it deliberately does NOT touch:

  Critic weights are kept. A linear fit to contradictory labels shrinks toward the mean rather
  than committing to a confident wrong answer, and at full pace TD moves it in a few hundred
  windows. Throwing them away also throws away the seven cost terms -- energy, heat, pressure,
  battery, breach, effort -- whose labels were never in question.

  Residual cells are kept. `metrics()` stores {p95, jank, energy, temp, cpuPsi}; only jank is
  cadence-derived, the surprise detector keys on p95, and each cell recency-weights at
  1/min(32, count). Discarding 1500 measured contexts to fix one of five metrics is a bad trade.

  The policy prior is kept: bounded logits with a 15% exploration floor, and its `touched`
  counters are validated against the header's update count, so it cannot be zeroed piecemeal.

Usage:  python tools/brain_surgery.py --drop-replay --reset-evidence
        python tools/brain_surgery.py --file model --out fixed --drop-replay   # offline
"""
import argparse
import subprocess
import sys
from pathlib import Path

DATA = "/data/adb/m54tuner"
MODEL = DATA + "/adaptive_model"
STAGE = "/data/local/tmp/adaptive_model"


def fnv1a(payload):
    # Must match m54::hash in core.cpp exactly, or the file fails its own checksum on load.
    value = 14695981039346656037
    for byte in payload:
        value = ((value ^ byte) * 1099511628211) % (1 << 64)
    return value


def adb(prefix, *args, binary=False):
    result = subprocess.run(prefix + list(args), capture_output=True)
    if result.returncode != 0:
        sys.exit("adb failed: {}\n{}".format(" ".join(args), result.stderr.decode(errors="replace")))
    return result.stdout if binary else result.stdout.decode(errors="replace")


def repair(data, drop_replay, reset_evidence):
    marker = data.rfind(b"CHECK ")
    if marker < 0:
        sys.exit("not a brain file: no CHECK trailer")
    payload = data[:marker]
    if fnv1a(payload) != int(data[marker + 6:].split()[0]):
        sys.exit("refusing to edit a brain that already fails its own checksum")

    lines = payload.split(b"\n")
    kept, dropped, evidence = [], 0, None
    for line in lines:
        if drop_replay and line.startswith(b"R "):
            dropped += 1
            continue
        if reset_evidence and line.startswith(b"U "):
            evidence = int(line.split()[1])
            line = b"U 0"
        kept.append(line)
    # The header's replay-seen counter is ignored on load when no samples follow it, so it needs
    # no adjustment here; every other header field describes state this script leaves alone.
    fixed = b"\n".join(kept)
    return fixed + b"CHECK " + str(fnv1a(fixed)).encode() + b"\n", dropped, evidence


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--drop-replay", action="store_true")
    parser.add_argument("--reset-evidence", action="store_true")
    parser.add_argument("--file", type=Path, help="operate on a local file instead of the device")
    parser.add_argument("--out", type=Path, help="where to write, with --file")
    parser.add_argument("--device", default="", help="adb serial, when more than one is attached")
    args = parser.parse_args()
    if not (args.drop_replay or args.reset_evidence):
        sys.exit("nothing to do: pass --drop-replay and/or --reset-evidence")

    if args.file:
        fixed, dropped, evidence = repair(args.file.read_bytes(), args.drop_replay, args.reset_evidence)
        (args.out or args.file).write_bytes(fixed)
        print("dropped {} replay samples, evidence counter was {}".format(dropped, evidence))
        return 0

    prefix = ["adb"] + (["-s", args.device] if args.device else [])
    # KernelSU keeps modules under its own root; the module's own installer makes the same test.
    scripts = adb(prefix, "shell",
                  "su 0 sh -c 'b=/data/adb/modules; [ ! -d /data/adb/ksu/modules ] || "
                  "b=/data/adb/ksu/modules; echo $b/m54tuner/scripts'").strip()
    if not scripts.endswith("/m54tuner/scripts"):
        sys.exit("could not locate the module scripts directory: " + scripts)
    # Stop first. The daemon rewrites the model every 60 s and again on exit, so editing it
    # underneath a running engine loses the edit at best.
    adb(prefix, "shell", "su 0 sh {}/adaptive_stop.sh".format(scripts))
    adb(prefix, "shell", "su 0 sh -c 'cp {0} {0}.pre-surgery; cp {0} {1}; chmod 644 {1}'".format(MODEL, STAGE))
    local = Path("adaptive_model.pulled")
    adb(prefix, "pull", STAGE, str(local))

    fixed, dropped, evidence = repair(local.read_bytes(), args.drop_replay, args.reset_evidence)
    patched = Path("adaptive_model.patched")
    patched.write_bytes(fixed)
    adb(prefix, "push", str(patched), STAGE)
    adb(prefix, "shell", "su 0 sh -c 'cp {0} {1}; chmod 600 {1}'".format(STAGE, MODEL))
    local.unlink(missing_ok=True)
    patched.unlink(missing_ok=True)

    adb(prefix, "shell", "su 0 sh {}/adaptive_start.sh".format(scripts))
    print("dropped {} replay samples, evidence counter was {}".format(dropped, evidence))
    print("backup kept at {}.pre-surgery".format(MODEL))
    print("the engine reloads it on the next window; check `replay=` and `confidence=` in status")
    return 0


if __name__ == "__main__":
    sys.exit(main())
