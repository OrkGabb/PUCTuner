"""Does paging cost frames on THIS device? Reads adaptive_history.csv and answers with data.

The paging channel is deliberately excluded from the objective until it demonstrates correlation
with observed stutter here. This is the report that settles that, and the one that fits the two
provisional feature scales in core.cpp to measured windows instead of to a second guess.

Usage:  python tools/paging_report.py                 # pulls the history off the device
        python tools/paging_report.py --file h.csv    # or reads a copy already on disk
"""
import argparse
import csv
import math
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

DATA = "/data/adb/m54tuner"
CHANNELS = ("major_faults_s", "swap_in_s", "file_refault_s")
TARGETS = ("p95_ms", "jank", "deficit")


def pull(device):
    """Copy both history generations off the device, oldest first."""
    prefix = ["adb"] + (["-s", device] if device else [])
    texts = []
    for name in ("adaptive_history.csv.1", "adaptive_history.csv"):
        stage = "/data/local/tmp/" + name
        subprocess.run(
            prefix + ["shell", "su 0 sh -c 'cp {0}/{1} {2} && chmod 644 {2}'".format(DATA, name, stage)],
            capture_output=True, text=True)
        with tempfile.TemporaryDirectory() as tmp:
            local = Path(tmp) / name
            got = subprocess.run(prefix + ["pull", stage, str(local)], capture_output=True, text=True)
            if got.returncode == 0 and local.exists():
                texts.append(local.read_text(encoding="utf-8", errors="replace"))
    return texts


def load(texts):
    rows = []
    for text in texts:
        for row in csv.DictReader(text.splitlines()):
            if "file_refault_s" not in row:
                continue  # written before the paging channel existed
            try:
                record = {k: float(row[k]) for k in CHANNELS + TARGETS + ("energy", "temp")}
            except (TypeError, ValueError):
                continue
            record["regime"] = row.get("regime", "")
            rows.append(record)
    return rows


def pearson(xs, ys):
    if len(xs) < 8:
        return None
    mx, my = statistics.fmean(xs), statistics.fmean(ys)
    sx = math.sqrt(sum((x - mx) ** 2 for x in xs))
    sy = math.sqrt(sum((y - my) ** 2 for y in ys))
    if sx == 0 or sy == 0:
        return None
    return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / (sx * sy)


def ranks(values):
    order = sorted(range(len(values)), key=lambda i: values[i])
    out = [0.0] * len(values)
    i = 0
    while i < len(order):
        j = i
        while j + 1 < len(order) and values[order[j + 1]] == values[order[i]]:
            j += 1
        for k in range(i, j + 1):
            out[order[k]] = (i + j) / 2 + 1
        i = j + 1
    return out


def spearman(xs, ys):
    return None if len(xs) < 8 else pearson(ranks(xs), ranks(ys))


def partial(xs, ys, zs):
    # Rank correlation of xs with ys once zs -- `energy`, the load proxy -- is held constant.
    # Without it every channel here correlates with stutter simply because a heavy scene produces
    # more of everything at once, which is the confound most likely to talk someone into a weight
    # this data does not support.
    if len(xs) < 8:
        return None
    rx, ry, rz = ranks(xs), ranks(ys), ranks(zs)
    rxy, rxz, ryz = pearson(rx, ry), pearson(rx, rz), pearson(ry, rz)
    if rxy is None or rxz is None or ryz is None:
        return None
    spread = math.sqrt(max(1e-12, (1 - rxz ** 2) * (1 - ryz ** 2)))
    return (rxy - rxz * ryz) / spread


def quantile(values, q):
    ordered = sorted(values)
    return ordered[min(len(ordered) - 1, int(q * len(ordered)))]


def stratified(render):
    # The strongest statement observational data can make here: split each load band into its
    # quiet and noisy halves by swap-in. If paging only tracked scene weight, the halves of a
    # band would look alike.
    energies = sorted(r["energy"] for r in render)
    cuts = [energies[int(q * len(energies))] for q in (.25, .5, .75)]
    print("{:<11}{:>6}{:>11}{:>11}{:>12}{:>12}".format(
        "load band", "n", "p95 quiet", "p95 noisy", "jank quiet", "jank noisy"))
    for band in range(4):
        group = [r for r in render if sum(r["energy"] >= cut for cut in cuts) == band]
        if len(group) < 12:
            print("{:<11}{:>6}   too few windows".format(band, len(group)))
            continue
        middle = statistics.median(r["swap_in_s"] for r in group)
        quiet = [r for r in group if r["swap_in_s"] <= middle]
        noisy = [r for r in group if r["swap_in_s"] > middle]
        if not quiet or not noisy:
            continue
        print("{:<11}{:>6}{:>11.1f}{:>11.1f}{:>12.3f}{:>12.3f}".format(
            band, len(group),
            statistics.median(r["p95_ms"] for r in quiet),
            statistics.median(r["p95_ms"] for r in noisy),
            statistics.median(r["jank"] for r in quiet),
            statistics.median(r["jank"] for r in noisy)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--file", type=Path, help="a history CSV already on disk")
    parser.add_argument("--device", default="", help="adb serial, when more than one is attached")
    args = parser.parse_args()

    texts = [args.file.read_text(encoding="utf-8", errors="replace")] if args.file else pull(args.device)
    rows = [r for r in load(texts) if r["file_refault_s"] >= 0]
    if not rows:
        print("no windows with a measured paging rate yet -- the channel needs run time, not a fix")
        return 1

    render = [r for r in rows if r["regime"] == "render"]
    print("windows with paging measured: {}   of those rendering: {}".format(len(rows), len(render)))
    print("")
    print("rates per second, over measured windows")
    print("{:<18}{:>12}{:>12}{:>12}{:>12}".format("channel", "median", "p90", "p99", "max"))
    for key in CHANNELS:
        vals = [r[key] for r in rows]
        print("{:<18}{:>12.1f}{:>12.1f}{:>12.1f}{:>12.1f}".format(
            key, quantile(vals, .5), quantile(vals, .9), quantile(vals, .99), max(vals)))
    print("")
    print("A feature scale belongs near p99: below it the channel saturates and stops carrying")
    print("information, far above it every ordinary window reads as zero.")
    print("  scales currently compiled into core.cpp -- swap_in 8000, file_refault 45000")
    print("")

    if len(render) < 8:
        print("not enough rendering windows yet; play for a while and run this again")
        return 0

    energy = [r["energy"] for r in render]
    views = (("pearson", lambda x, y: pearson(x, y)),
             ("rank", lambda x, y: spearman(x, y)),
             ("rank, load held", lambda x, y: partial(x, y, energy)))
    for label, measure in views:
        print("{:<18}{:>12}{:>12}{:>12}".format(label, "vs p95_ms", "vs jank", "vs deficit"))
        for key in CHANNELS:
            xs = [r[key] for r in render]
            cells = ""
            for target in TARGETS:
                value = measure(xs, [row[target] for row in render])
                cells += "         n/a" if value is None else "{:>12.3f}".format(value)
            print("  {:<16}{}".format(key, cells))
        print("")

    print("stutter of the noisy half against the quiet half, inside bands of load")
    stratified(render)
    print("")
    print("Pearson is dominated by the tail here -- these rates span four orders of magnitude --")
    print("so read the rank rows, and read them knowing a heavy scene moves paging AND p95 at")
    print("once. Under |0.3| with load held is no reason to touch the objective; above it is a")
    print("reason to run an A/B, never a reason to add a weight straight out of this table.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
