"""Attribute a long-session frame collapse: external throttle, engine backoff, or neither.

Reads tools/thermal_watch.sh's log and joins it to the engine's own windows on `at_mono`, the
CLOCK_MONOTONIC both files carry. Answers three questions that the history alone cannot separate,
because the history records the OUTCOME and not who caused it.

  1. CEILING   Did anything lower a ceiling? The engine only ever raises floors, so a falling
               scaling_max_freq, gpu_max_clock, MIF max_freq, or a rising cooling_device state is
               somebody else. Reported as "first seen at" plus the battery temperature there,
               because the question is not whether a throttle exists but what it is indexed to.

  2. POWER     Real milliwatts, current_now x voltage_now. A withdrawn power budget must show
               falling power at constant requested effort; if power holds while frames fall, the
               budget theory is wrong and the workload got heavier instead.

  3. REACH     What the engine asked for against what sysfs reads back. `own_p4min` is the value
               the engine last wrote to the big cluster's floor per its write-ahead journal, and
               `p4_min` is what the node actually holds. -1 in the own_ column means the engine
               owns nothing there, which is a different fact from asking for zero.

Read the sections in that order. A ceiling that never moves makes section 1 the answer by itself.

Usage:  python tools/thermal_report.py
        python tools/thermal_report.py --watch w.log --history h.csv
"""
import argparse
import math
import statistics
import subprocess
import sys
import tempfile
from pathlib import Path

DATA = "/data/adb/m54tuner"
WATCH_LOG = "/data/local/tmp/thermal_watch.log"
# Ceilings and cooling states: nothing the engine is allowed to touch. Any movement here is external.
CEILINGS = ("p0_max", "p4_max", "g_max", "mif_max")
COOLERS = ("cd_isp", "cd_lit", "cd_big", "cd_dev", "cd_gpu")
# Floors: the engine's own actuator surface, cross-checked against its journal.
FLOORS = (("p0_min", "own_p0min"), ("p4_min", "own_p4min"), ("g_min", "own_gmin"))


def pull(device, remote, name):
    prefix = ["adb"] + (["-s", device] if device else [])
    stage = "/data/local/tmp/pulled_" + name
    subprocess.run(prefix + ["shell", "su 0 sh -c 'cp {0} {1} && chmod 644 {1}'".format(remote, stage)],
                   capture_output=True, text=True)
    with tempfile.TemporaryDirectory() as tmp:
        local = Path(tmp) / name
        got = subprocess.run(prefix + ["pull", stage, str(local)], capture_output=True, text=True)
        return local.read_text(encoding="utf-8", errors="replace") if got.returncode == 0 and local.exists() else ""


def load_watch(text):
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
            continue
        try:
            rows.append({k: float(v) for k, v in zip(header, parts)})
        except ValueError:
            continue
    for row in rows:
        # current_now is milliamps on this device and negative while discharging; voltage is uV.
        row["mw"] = abs(row["batt_mA"]) * row["batt_uV"] / 1e6 if row["batt_uV"] > 0 else float("nan")
        row["batt_c"] = row["batt_mC"] / 10
    return rows


def load_history(texts):
    rows = []
    for text in texts:
        lines = text.splitlines()
        if not lines:
            continue
        header = lines[0].split(",")
        if "at" not in header or "action" not in header:
            continue
        for line in lines[1:]:
            parts = line.split(",")
            if len(parts) != len(header):
                continue
            row = dict(zip(header, parts))
            try:
                record = {k: float(row[k]) for k in ("at", "p95_ms", "frames", "budget", "temp",
                                                     "battery_temp", "action")}
            except (KeyError, ValueError):
                continue
            record["regime"] = row.get("regime", "")
            record["cadence"] = row.get("cadence", "")
            rows.append(record)
    rows.sort(key=lambda r: r["at"])
    for i, row in enumerate(rows):
        span = row["at"] - rows[i - 1]["at"] if i else 0.0
        row["fps"] = row["frames"] / span if 0.5 < span < 30 else float("nan")
        row["levels"] = levels(int(row["action"]))
        row["effort"] = sum(row["levels"])
    return rows


def levels(action):
    """id = l0 + 5*l1 + 25*l2 + 125*l3 over [cpu floor, gpu window, MIF floor, PELT]."""
    out = []
    for _ in range(4):
        out.append(action % 5)
        action //= 5
    return out


def phase(rows, key, lo, hi):
    vals = [r[key] for r in rows if lo <= r["at_mono"] <= hi and not math.isnan(r[key]) and r[key] >= 0]
    return statistics.median(vals) if vals else float("nan")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--watch", type=Path)
    parser.add_argument("--history", type=Path)
    parser.add_argument("--device", default="")
    args = parser.parse_args()

    text = args.watch.read_text(encoding="utf-8", errors="replace") if args.watch else pull(args.device, WATCH_LOG, "watch.log")
    if args.history:
        histories = [args.history.read_text(encoding="utf-8", errors="replace")]
    else:
        histories = [t for t in (pull(args.device, DATA + "/adaptive_history.csv.1", "h1.csv"),
                                 pull(args.device, DATA + "/adaptive_history.csv", "h.csv")) if t]
    rows = load_watch(text)
    if len(rows) < 30:
        print("watch log too short -- is it running?  touch /data/local/tmp/thermal_on")
        return 1
    history = load_history(histories)
    lo, hi = rows[0]["at_mono"], rows[-1]["at_mono"]
    game = [r for r in history if lo <= r["at"] <= hi and r["regime"] == "render"]
    minutes = (rows[-1]["at_boot"] - rows[0]["at_boot"]) / 60
    print("watch samples {}  over {:.1f} min   engine render windows joined {}".format(
        len(rows), minutes, len(game)))
    if not game:
        # Sections 1 and 3 read hardware and the engine's journal; neither needs a frame. Only the
        # engine half of section 2 does. Bailing out here would throw away a perfectly good answer
        # about what the hardware did whenever the game happened not to be in the foreground.
        print("no rendering windows joined -- hardware sections still stand, engine rows omitted")
    print("")

    print("1. CEILINGS -- the engine writes none of these, so anything that moves is external")
    baseline = rows[0]
    moved = False
    for key in CEILINGS:
        values = [r[key] for r in rows if r[key] > 0]
        if not values:
            print("  {:<10} not readable".format(key))
            continue
        top = max(values)
        below = [r for r in rows if 0 < r[key] < top]
        if not below:
            print("  {:<10} held at {:.0f} for the whole session".format(key, top))
            continue
        moved = True
        first = below[0]
        print("  {:<10} {:.0f} -> {:.0f}, first drop at {:.1f} min, battery {:.1f} C, die BIG {:.1f} C, {:.0f}% of samples".format(
            key, top, min(r[key] for r in below), (first["at_boot"] - rows[0]["at_boot"]) / 60,
            first["batt_c"], first["z_BIG"] / 1000, 100 * len(below) / len(rows)))
    for key in COOLERS:
        values = [r[key] for r in rows if r[key] >= 0]
        if not values or max(values) == min(values):
            print("  {:<10} constant at {:.0f}".format(key, values[0] if values else -1))
            continue
        moved = True
        rise = [r for r in rows if r[key] > values[0]]
        first = rise[0] if rise else rows[0]
        print("  {:<10} {:.0f} -> {:.0f}, first rise at {:.1f} min, battery {:.1f} C, die BIG {:.1f} C".format(
            key, min(values), max(values), (first["at_boot"] - rows[0]["at_boot"]) / 60,
            first["batt_c"], first["z_BIG"] / 1000))
    print("  => {}".format("an external limiter acted; the columns above say when and at what temperature"
                           if moved else "NOTHING external moved. The frames were not taken by a throttle."))
    print("")

    print("2. POWER and temperature, in thirds of the session")
    cut = [lo + (hi - lo) * i / 3 for i in range(4)]
    print("  {:<10}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>8}".format(
        "third", "mW", "batt_C", "BIG_C", "G3D_C", "p4_cur", "gpu_cur", "gpu%"))
    for i in range(3):
        part = [r for r in rows if cut[i] <= r["at_mono"] <= cut[i + 1]]
        if not part:
            continue
        def med(key, scale=1.0):
            vals = [r[key] * scale for r in part if not math.isnan(r[key]) and r[key] >= 0]
            return statistics.median(vals) if vals else float("nan")
        print("  {:<10}{:>9.0f}{:>9.1f}{:>9.1f}{:>9.1f}{:>9.0f}{:>9.0f}{:>8.0f}".format(
            ["first", "second", "third"][i], med("mw"), med("batt_c"), med("z_BIG", .001),
            med("z_G3D", .001), med("p4_cur"), med("g_cur"), med("g_busy")))
    if not game:
        print("  (no engine rows: nothing was rendering while this ran)")
    else:
        print("  {:<10}{:>9}{:>9}{:>9}{:>9}{:>9}{:>9}{:>8}".format(
            "engine", "p95_ms", "fps", "effort", "cpu_L", "gpu_L", "mif_L", "budget"))
        for i in range(3):
            part = [r for r in game if cut[i] <= r["at"] <= cut[i + 1]]
            if not part:
                continue
            def med(fn):
                vals = [fn(r) for r in part if not math.isnan(fn(r))]
                return statistics.median(vals) if vals else float("nan")
            print("  {:<10}{:>9.1f}{:>9.1f}{:>9.1f}{:>9.1f}{:>9.1f}{:>9.1f}{:>8.3f}".format(
                ["first", "second", "third"][i], med(lambda r: r["p95_ms"]), med(lambda r: r["fps"]),
                med(lambda r: r["effort"]), med(lambda r: r["levels"][0]),
                med(lambda r: r["levels"][1]), med(lambda r: r["levels"][2]),
                med(lambda r: r["budget"])))
    print("  Power falling while effort holds is a withdrawn budget. Power holding while frames")
    print("  fall is a heavier scene, and no knob on this device will fix that.")
    print("")

    print("3. REACH -- what the engine asked for vs what the node actually holds")
    print("  {:<12}{:>10}{:>12}{:>12}{:>10}{:>26}".format(
        "node", "owned %", "asked med", "actual med", "match %", "actual range"))
    for node, own in FLOORS:
        held = [r for r in rows if r[own] > 0]
        pct = 100 * len(held) / len(rows)
        actual = [r[node] for r in rows if r[node] > 0]
        if not held:
            print("  {:<12}{:>10.0f}{:>12}{:>12.0f}{:>10}{:>26}".format(
                node, pct, "never", statistics.median(actual) if actual else -1, "n/a",
                "{:.0f}-{:.0f}".format(min(actual), max(actual)) if actual else "-"))
            continue
        agree = sum(1 for r in held if abs(r[node] - r[own]) < 1)
        print("  {:<12}{:>10.0f}{:>12.0f}{:>12.0f}{:>10.0f}{:>26}".format(
            node, pct, statistics.median([r[own] for r in held]),
            statistics.median([r[node] for r in held]), 100 * agree / len(held),
            "{:.0f}-{:.0f}".format(min(actual), max(actual)) if actual else "-"))
    owned_n = [r["owned_n"] for r in rows]
    print("  journal lines (1 = header only, engine owns nothing): min {:.0f} median {:.0f} max {:.0f}".format(
        min(owned_n), statistics.median(owned_n), max(owned_n)))
    print("  A node owned 0% of the time while p95 climbs is an axis the planner is not using.")
    print("  A node owned but whose actual value never matches what was asked is an axis with no reach.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
