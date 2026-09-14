"""Paired verdict on the vm.watermark_scale_factor A/B, joined to the engine's own windows.

The A/B script alternates one knob inside a single play session and logs kernel reclaim counters.
It cannot see frames. The engine's adaptive_history.csv can, and stamps every window with seconds
since boot, the same clock /proc/uptime gives the A/B log in `at_boot`, so the two join without a
global offset. `at_mono` is scraped from the engine's status file and is only read here as proof
the daemon was alive for that block; the windowing is done on `at_boot`, because a monotonic key
loses every second the phone spent suspended and would offset the two files by all of them.

Why the arithmetic looks the way it does. An alternating design inside one session is confounded
with everything that drifts across the session -- the scene, the die temperature, the state of
charge, the engine's own learning. The estimator here is the drift-cancelling one:

  triplet       for consecutive blocks x, y, z where y is the opposite arm, y - (x + z) / 2.
                Cancels any LINEAR drift outright, which is the leading term over 30 minutes.
                A plain block-to-block difference cancels a constant offset and not a trend, and
                a trend is exactly what round 1 could not rule out.

The sign test is on those contrasts and is exact (no scipy): under the null the sign of each
contrast is a fair coin, so p is the two-sided binomial tail. Five contrasts cannot reach p < .05
however clean the direction looks -- that is the honest reason round 1 was not a verdict, and the
reason this round wants ten or more.

Usage:  python tools/ab_report.py                       # pulls both files off the device
        python tools/ab_report.py --ab a.log --history h.csv
"""
import argparse
import math
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

DATA = "/data/adb/m54tuner"
AB_LOG = "/data/local/tmp/ab_wsf2.log"
# Seconds of each arm thrown away as settling. Raising a watermark takes effect on the next
# allocation, but the reclaim it provokes, and the frames behind it, need a moment to settle.
SETTLE = 30.0
# Engine metrics, and which direction is better. Rates come straight out of the history.
METRICS = (("p95_ms", "lower"), ("fps", "higher"), ("jank", "lower"), ("deficit", "lower"),
           ("major_faults_s", "lower"), ("file_refault_s", "lower"), ("swap_in_s", "lower"),
           ("mem_psi", "lower"), ("io_psi", "lower"))
# Counters read from the A/B log itself, differenced across a block into a per-second rate.
COUNTERS = ("allocstall", "pgsteal_direct", "pgsteal_kswapd", "pswpin", "pswpout", "ws_refault")


def pull(device, remote, name):
    prefix = ["adb"] + (["-s", device] if device else [])
    stage = "/data/local/tmp/pulled_" + name
    subprocess.run(prefix + ["shell", "su 0 sh -c 'cp {0} {1} && chmod 644 {1}'".format(remote, stage)],
                   capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / name
        got = subprocess.run(prefix + ["pull", stage, str(local)], capture_output=True, text=True)
        if got.returncode != 0 or not local.exists():
            return ""
        return local.read_text(encoding="utf-8", errors="replace")


def load_ab(text):
    """Rows of the A/B log. Comment lines carry the arm switches and are not samples."""
    rows, header = [], None
    for line in text.splitlines():
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if header is None:
            header = parts
            continue
        if len(parts) != len(header):
            continue  # a torn append caught while the file was being copied
        row = {}
        ok = True
        for key, value in zip(header, parts):
            if key == "arm":
                row[key] = value
                continue
            try:
                row[key] = float(value)
            except ValueError:
                ok = False
                break
        if ok:
            rows.append(row)
    return rows


def blocks(rows):
    """Contiguous runs of one arm, keyed by the engine clock, settling trimmed off the front."""
    out = []
    for row in rows:
        if out and out[-1]["arm"] == row["arm"]:
            out[-1]["rows"].append(row)
        else:
            out.append({"arm": row["arm"], "value": row["value"], "rows": [row]})
    kept = []
    for block in out:
        inner = block["rows"]
        if inner[0].get("at_mono", -1) <= 0:
            continue  # the engine was not writing its status; this block cannot be joined
        start = inner[0]["at_boot"] + SETTLE
        end = inner[-1]["at_boot"]
        if end - start < 45:
            continue  # too short to hold enough windows to median
        block["start"], block["end"] = start, end
        block["usable"] = [r for r in inner if r["at_boot"] >= start]
        if len(block["usable"]) < 5:
            continue
        kept.append(block)
    return kept


def load_history(texts):
    rows = []
    for text in texts:
        lines = text.splitlines()
        if not lines:
            continue
        header = lines[0].split(",")
        if "at" not in header or "file_refault_s" not in header:
            continue
        for line in lines[1:]:
            parts = line.split(",")
            if len(parts) != len(header):
                continue
            row = dict(zip(header, parts))
            try:
                record = {k: float(row[k]) for k in
                          ("at", "frames", "p95_ms", "jank", "deficit", "mem_psi", "io_psi",
                           "major_faults_s", "swap_in_s", "file_refault_s")}
            except (KeyError, ValueError):
                continue
            record["regime"] = row.get("regime", "")
            rows.append(record)
    rows.sort(key=lambda r: r["at"])
    # The window length is not in the file; it is the gap to the previous window. Deriving fps from
    # `frames` without it silently assumes a window length the engine is free to change.
    for i, row in enumerate(rows):
        span = row["at"] - rows[i - 1]["at"] if i else 0.0
        row["fps"] = row["frames"] / span if 0.5 < span < 30 else float("nan")
    return rows


def quantile(values, q):
    ordered = sorted(values)
    if not ordered:
        return float("nan")
    pos = q * (len(ordered) - 1)
    low = int(math.floor(pos))
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (pos - low)


def summarise(block, history):
    windows = [r for r in history
               if block["start"] <= r["at"] <= block["end"] and r["regime"] == "render"]
    block["windows"] = len(windows)
    stats = {}
    for name, _ in METRICS:
        vals = [r[name] for r in windows if not math.isnan(r[name]) and r[name] >= 0]
        stats[name] = statistics.median(vals) if vals else float("nan")
    vals = [r["p95_ms"] for r in windows if r["p95_ms"] >= 0]
    stats["p95_p95"] = quantile(vals, .95) if vals else float("nan")
    stats["over100"] = (sum(1 for v in vals if v > 100) / len(vals)) if vals else float("nan")
    # Reclaim counters are monotonic; a block's rate is a difference over its own span.
    usable = block["usable"]
    span = usable[-1]["at_boot"] - usable[0]["at_boot"] if len(usable) > 1 else 0.0
    for key in COUNTERS:
        stats[key + "_s"] = ((usable[-1][key] - usable[0][key]) / span
                             if span > 5 and key in usable[0] else float("nan"))
    direct = stats.get("pgsteal_direct_s", float("nan"))
    kswapd = stats.get("pgsteal_kswapd_s", float("nan"))
    total = direct + kswapd
    stats["direct_share"] = direct / total if total > 0 else float("nan")
    block["stats"] = stats
    return block


def contrasts(good, name, direction):
    """Triplet contrasts y - (x+z)/2, signed so POSITIVE always means the test arm (B) was better."""
    out = []
    for i in range(1, len(good) - 1):
        x, y, z = good[i - 1], good[i], good[i + 1]
        if x["arm"] == y["arm"] or z["arm"] == y["arm"]:
            continue
        a, b, c = x["stats"][name], y["stats"][name], z["stats"][name]
        if any(math.isnan(v) for v in (a, b, c)):
            continue
        middle = b - (a + c) / 2                          # middle arm minus its flanks, drift out
        delta = middle if y["arm"] == "B" else -middle     # always B minus A
        if direction == "lower":
            delta = -delta                                 # positive now always reads "B better"
        out.append((i, y["arm"], delta))
    return out


def binomial_two_sided(hits, n):
    """Exact two-sided sign test. No scipy on this machine, and none needed for n this small."""
    if n == 0:
        return float("nan")
    extreme = min(hits, n - hits)
    tail = sum(math.comb(n, k) for k in range(0, extreme + 1))
    return min(1.0, 2 * tail / (2 ** n))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ab", type=Path, help="an A/B log already on disk")
    parser.add_argument("--history", type=Path, help="an adaptive_history.csv already on disk")
    parser.add_argument("--device", default="", help="adb serial, when more than one is attached")
    args = parser.parse_args()

    ab_text = (args.ab.read_text(encoding="utf-8", errors="replace") if args.ab
               else pull(args.device, AB_LOG, "ab.log"))
    if args.history:
        history_texts = [args.history.read_text(encoding="utf-8", errors="replace")]
    else:
        history_texts = [t for t in (pull(args.device, DATA + "/adaptive_history.csv.1", "hist1.csv"),
                                     pull(args.device, DATA + "/adaptive_history.csv", "hist.csv")) if t]
    if not ab_text.strip():
        print("no A/B log -- is the run started?  touch /data/local/tmp/ab_on")
        return 1

    rows = load_ab(ab_text)
    history = load_history(history_texts)
    good = [b for b in (summarise(b, history) for b in blocks(rows)) if b["windows"] >= 5]
    span = (rows[-1]["at_boot"] - rows[0]["at_boot"]) / 60 if len(rows) > 1 else 0
    print("A/B samples {}  over {:.1f} min   engine windows loaded {}".format(len(rows), span, len(history)))
    print("blocks with a joinable clock and rendering windows in them: {}".format(len(good)))
    if len(good) < 3:
        print("not enough blocks yet -- keep playing; a verdict needs ten or more contrasts")
        return 0
    print("")

    print("per block   (A = factory wsf 10, B = wsf 100; medians over RENDERING windows)")
    print("{:>3} {:>4} {:>6} {:>5} {:>8} {:>8} {:>6} {:>9} {:>10} {:>8} {:>8} {:>8}".format(
        "#", "arm", "min", "wins", "p95_med", "p95_p95", "fps", "majflt/s", "refault/s",
        "stall/s", "direct%", "mem_psi"))
    for i, b in enumerate(good):
        s = b["stats"]
        print("{:>3} {:>4} {:>6.1f} {:>5} {:>8.1f} {:>8.1f} {:>6.1f} {:>9.0f} {:>10.0f} {:>8.2f} {:>8.1%} {:>8.2f}".format(
            i, b["arm"], (b["end"] - b["start"]) / 60, b["windows"], s["p95_ms"], s["p95_p95"],
            s["fps"], s["major_faults_s"], s["file_refault_s"], s["allocstall_s"],
            s["direct_share"], s["mem_psi"]))
    print("")

    print("drift-cancelling triplet contrasts, signed so POSITIVE = wsf 100 was better")
    print("{:<18}{:>10}{:>10}{:>6}{:>10}{:>9}".format(
        "metric", "mean", "median", "n", "n better", "sign p"))
    verdict = []
    for name, direction in METRICS:
        cs = contrasts(good, name, direction)
        if not cs:
            continue
        deltas = [d for _, _, d in cs]
        hits = sum(1 for d in deltas if d > 0)
        p = binomial_two_sided(hits, len(deltas))
        print("{:<18}{:>10.3f}{:>10.3f}{:>6}{:>10}{:>9.3f}".format(
            name, statistics.fmean(deltas), statistics.median(deltas), len(deltas), hits, p))
        verdict.append((name, statistics.fmean(deltas), hits, len(deltas), p))
    print("")
    n = max((v[3] for v in verdict), default=0)
    print("with {} contrasts the smallest reachable two-sided p is {:.3f}".format(
        n, binomial_two_sided(n, n) if n else float("nan")))
    if n < 5:
        print("that cannot separate a real effect from a coin -- this is a direction, not a verdict")
    p95 = next((v for v in verdict if v[0] == "p95_ms"), None)
    if p95:
        print("")
        print("p95_ms decides it: mean {:+.2f} ms in favour of {}, {}/{} contrasts agree".format(
            p95[1], "wsf 100" if p95[1] > 0 else "factory", p95[2], p95[3]))
    print("A knob is worth shipping when the direction survives the contrast AND the reclaim")
    print("counters move the way the mechanism says they must: allocstall and direct% down.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
