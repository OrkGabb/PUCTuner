"""Frozen on-device crossover: observe, full controller, controller without PELT.

Uses presentation timestamps, never frames/6. Saves raw data before reporting; rejects
handover windows, wrong apps, stale timestamps and unexpected configuration changes.
Restores the exact starting config if it still owns the test configuration.
"""
import argparse
import collections
import datetime
import json
import math
from pathlib import Path
import shlex
import statistics
import subprocess
import time

DATA = "/data/adb/m54tuner"
ARMS = {"observe": ("observe", "1"), "full": ("active", "1"), "no_pelt": ("active", "0")}


def parse(text):
    return dict(line.split("=", 1) for line in text.splitlines() if "=" in line and not line.startswith("#"))


def root(device, script):
    result = subprocess.run(
        ["adb", "-s", device, "shell", "su -c 'sh -s'"], input=script,
        text=True, capture_output=True, timeout=20, encoding="utf-8",
    )
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or result.stdout.strip())
    return result.stdout.replace("\r", "")


def read_config(device):
    return root(device, f"cat {DATA}/config\n")


def config_for(base, arm):
    mode, pelt = ARMS[arm]
    updates = {"adaptive_mode": mode, "adaptive_learning": "0", "adaptive_pelt": pelt}
    lines = [line for line in base.splitlines() if line.partition("=")[0] not in updates]
    return "\n".join(lines + [f"{key}={value}" for key, value in updates.items()]) + "\n"


def write_config(device, value):
    root(device, f"set -e\numask 077\nprintf %s {shlex.quote(value)} > {DATA}/config.ablation.tmp\n"
         f"mv {DATA}/config.ablation.tmp {DATA}/config\n")


def snapshot(device, app):
    text = root(device, f"""set -e
cat {DATA}/adaptive_status
printf 'uptime='; cut -d ' ' -f 1 /proc/uptime
printf 'actual_pelt='; cat /proc/sys/kernel/sched_pelt_multiplier
printf 'actual_swappiness='; cat /proc/sys/vm/swappiness
printf 'actual_readahead='; cat /sys/block/sda/queue/read_ahead_kb
printf 'config_mode='; sed -n 's/^adaptive_mode=//p' {DATA}/config
printf 'config_learning='; sed -n 's/^adaptive_learning=//p' {DATA}/config
printf 'config_pelt='; sed -n 's/^adaptive_pelt=//p' {DATA}/config
game_pid=$(pidof {shlex.quote(app)} | cut -d ' ' -f 1)
if [ -n "$game_pid" ] && [ -r /proc/$game_pid/stat ]; then
  printf 'proc_stat='; cat /proc/$game_pid/stat
  printf 'read_bytes='; sed -n 's/^read_bytes: *//p' /proc/$game_pid/io
  printf 'swap_kb='; awk '/^VmSwap:/ {{print $2}}' /proc/$game_pid/status
fi
""")
    return parse(text)


def summarize(rows):
    result = {}
    for arm in ARMS:
        group = [row for row in rows if row.get("accepted") and row["arm"] == arm]
        if not group:
            result[arm] = {"windows": 0}
            continue
        span = sum(float(row["frame_time_ms"]) for row in group)
        count = sum(int(row["frames"]) for row in group)
        watts = [float(row["watts"]) for row in group if row.get("watts") not in (None, "unavailable")]
        result[arm] = {
            "windows": len(group), "fps": 1000 * count / span,
            "p95_mean_ms": statistics.mean(float(row["p95_ms"]) for row in group),
            "p95_median_ms": statistics.median(float(row["p95_ms"]) for row in group),
            "over_50ms_fraction": sum(int(row["slow_frames_50ms"]) for row in group) / count,
            "temp_mean_c": statistics.mean(float(row["temp"]) for row in group),
            "watts_mean": statistics.mean(watts) if watts else None,
            "actions": dict(collections.Counter(row["measured_action"] for row in group)),
            "pelt_values": dict(collections.Counter(row["actual_pelt"] for row in group)),
        }
    return result


def memory_delta(before, after):
    try:
        def stat(row):
            raw = row["proc_stat"]
            fields = raw[raw.rfind(")") + 2:].split()
            return raw.split(" ", 1)[0], fields[19], int(fields[9])
        a, b = stat(before), stat(after)
        seconds = float(after["uptime"]) - float(before["uptime"])
        faults = b[2] - a[2]
        reads = int(after["read_bytes"].strip()) - int(before["read_bytes"].strip())
        if a[:2] != b[:2] or seconds <= 0 or faults < 0 or reads < 0:
            return None
        return {"major_faults_per_second": faults / seconds,
                "read_mib_per_second": reads / seconds / 1048576,
                "swap_start_mib": int(before["swap_kb"]) / 1024,
                "swap_end_mib": int(after["swap_kb"]) / 1024}
    except (KeyError, ValueError, IndexError):
        return None


def report(path):
    rows = [json.loads(line) for line in (path / "samples.jsonl").read_text().splitlines()]
    report_data = {"aggregate": summarize(rows), "cycles": {},
                   "rejections": dict(collections.Counter(row.get("rejection") for row in rows if not row.get("accepted")))}
    for cycle in sorted({row["cycle"] for row in rows}):
        report_data["cycles"][cycle] = summarize([row for row in rows if row["cycle"] == cycle])
    if rows:
        report_data["memory"] = memory_delta(rows[0], rows[-1])
    (path / "report.json").write_text(json.dumps(report_data, indent=2) + "\n")
    print(json.dumps(report_data, indent=2), flush=True)
    return report_data


def run(args):
    out = args.out
    out.mkdir(parents=True, exist_ok=False)
    base = read_config(args.device)
    (out / "config.before").write_text(base)
    initial = snapshot(args.device, args.app)
    required = {"frame_start", "frame_end", "frame_time_ms", "measured_action", "pelt_allowed"}
    if not required <= initial.keys():
        raise RuntimeError("Install the telemetry correction before measuring")
    if initial.get("app") != args.app or initial.get("frames_valid") != "1":
        raise RuntimeError("The requested game is not producing valid frames")
    baseline_count = (initial["samples"], initial["windows"])
    expected = base
    seen = set()
    accepted = []
    (out / "metadata.json").write_text(json.dumps({
        "started": datetime.datetime.now().astimezone().isoformat(),
        "block_seconds": args.seconds, "cycles": args.cycles, "washout_seconds": 12,
        "app": args.app, "initial": initial,
        "arms": args.arms,
        "design": "Rotating mirrored order; frozen model; no scene synchronization",
    }, indent=2))
    complete = False
    try:
        with (out / "samples.jsonl").open("w", buffering=1) as stream:
            names = args.arms
            for cycle in range(1, args.cycles + 1):
                offset = (cycle - 1) % len(names)
                order = names[offset:] + names[:offset]
                for block, arm in enumerate(order + list(reversed(order)), 1):
                    if read_config(args.device) != expected:
                        raise RuntimeError("Config changed outside this measurement; preserving that edit")
                    expected = config_for(base, arm)
                    write_config(args.device, expected)
                    start = float(root(args.device, "cut -d ' ' -f 1 /proc/uptime\n").strip())
                    end = time.monotonic() + args.seconds
                    print(f"cycle={cycle}/{args.cycles} block={block}/{2 * len(names)} arm={arm}", flush=True)
                    while time.monotonic() < end:
                        row = snapshot(args.device, args.app)
                        row.update(arm=arm, cycle=cycle, block=block, block_start=start)
                        reason = ""
                        mode, pelt = ARMS[arm]
                        if (row.get("config_mode"), row.get("config_learning"), row.get("config_pelt")) != (mode, "0", pelt):
                            raise RuntimeError("Measurement configuration changed while sampling")
                        if (row.get("samples"), row.get("windows")) != baseline_count:
                            raise RuntimeError("The frozen model changed or the daemon restarted")
                        stamp = row.get("at")
                        if stamp in seen:
                            reason = "duplicate"
                        elif row.get("app") != args.app:
                            raise RuntimeError("Foreground app changed; stopping the measurement")
                        elif row.get("frames_valid") != "1":
                            reason = "no_frames"
                        elif row.get("mode") != mode or row.get("pelt_allowed") != ("0" if pelt == "0" else initial["pelt_allowed"]):
                            reason = "control_handover"
                        elif float(row.get("frame_start", 0)) < start + 12:
                            reason = "washout"
                        elif float(row.get("uptime", 0)) - float(stamp or 0) > 10:
                            reason = "stale"
                        elif arm == "no_pelt" and (int(row["measured_action"]) >= 125 or row["actual_pelt"] != initial["actual_pelt"]):
                            raise RuntimeError("PELT exclusion did not hold")
                        elif not math.isclose(float(row["frame_end"]) - float(row["frame_start"]),
                                              float(row["frame_time_ms"]) / 1000, abs_tol=.02):
                            reason = "frame_gap"
                        row["accepted"] = not reason
                        row["rejection"] = reason
                        stream.write(json.dumps(row) + "\n")
                        if stamp:
                            seen.add(stamp)
                        if not reason:
                            accepted.append(row)
                        time.sleep(min(2, max(0, end - time.monotonic())))
                    print(json.dumps(summarize(accepted)), flush=True)
        complete = True
    finally:
        restoration_error = None
        try:
            current = read_config(args.device)
            if current == expected:
                write_config(args.device, base)
                if read_config(args.device) != base:
                    raise RuntimeError("Starting config restoration failed verification")
                (out / "restoration.txt").write_text("Exact starting config restored.\n")
            else:
                (out / "restoration.txt").write_text("External config edit preserved; no overwrite.\n")
        except (RuntimeError, subprocess.SubprocessError, OSError) as error:
            restoration_error = error
            (out / "restoration.txt").write_text(f"Restoration not verified: {error}\n")
        (out / "completion.json").write_text(json.dumps({"complete": complete}) + "\n")
        report(out)
        if restoration_error is not None:
            raise RuntimeError("See restoration.txt; device restoration was not verified") from restoration_error


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--device", default="RQCW506Z0CX")
    parser.add_argument("--app", default="com.hottagames.nte")
    parser.add_argument("--seconds", type=int, default=60)
    parser.add_argument("--cycles", type=int, default=2)
    parser.add_argument("--arms", nargs="+", choices=list(ARMS), default=["observe", "full"])
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--report", action="store_true")
    args = parser.parse_args()
    if args.report:
        report(args.out)
    else:
        if args.seconds < 30 or args.cycles < 1:
            parser.error("Use at least 30 seconds per block and one cycle")
        run(args)
