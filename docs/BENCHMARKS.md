# PUCTuner — Benchmarks & Empirical Validation

This document records the empirical validation of PUCTuner across unit testing, closed-loop simulation, kernel eBPF scheduler telemetry, and hardware node verification.

---

## 1. Closed-Loop Simulation Benchmarks

Simulated over **900 windows per workload** (fixed 6 s steps in the simulator; the on-device
windows close on frame evidence between 4.5 s and 12 s), evaluated across **7 independent random
seeds** with synthetic noise, latency jitter, and thermal accumulation. **These are simulator
outputs, not device measurements**: the synthetic device is built to reward the controller's own
assumptions, so passing here shows the loop converges toward the objective it was given — not a
gain on the real M54. Do not quote point values below as empirical results; re-run
`tests/engine_sim.cpp` for the current numbers (they drift with seeds and toolchains, and the
phase test below already demonstrated a 7-seed/35-seed order inversion once).

### Scenario Results (illustrative, one 7-seed run — 2026-09-20)

| Scenario | Objective | Mean Effort (0–16) | Mean GPU Floor | Frame Latency p95 | Normalized Reward | Peak SoC Temp | Thermal Breaches |
|---|---|---:|---:|---:|---:|---:|---:|
| **Game / Responsive** | Responsive | 3.62 | 2.72 | 14.3 ms | +0.843 | 49.6°C | **0** |
| **Game / Flat (Unresponsive)** | No scaling benefit | 0.29 | 0.07 | 26.0 ms | +0.551 | 44.6°C | **0** |
| **Game / Hostile Heat** | Heavy heat accumulation | 2.02 | 1.71 | 18.8 ms | +0.770 | 71.0°C | **0** |
| **Powersave / Responsive** | High energy penalty | 1.65 | 1.45 | 19.9 ms | +0.534 | 40.3°C | **0** |
| **Balanced / Responsive** | Production default | 1.94 | 1.81 | 18.4 ms | +0.699 | 41.5°C | **0** |

### Key Findings:
1. **Refusal to Overspend:** When the workload ceases to benefit from higher frequencies (`Game/Flat`), the controller drops to near-zero effort, refusing to burn energy for zero frame pacing gain.
2. **Thermal Ceiling Respect:** In the hostile heat curve scenario, the controller dynamically trims hardware floors with **zero threshold breaches** (worst seed reported, not just the mean).

---

## 2. Dynamic Adaptation to Unannounced Workload Shifts

Evaluated in continuous execution where the underlying hardware responsiveness changes without notifying the controller (450 windows per phase, 21 seeds; looser bars than the static scenarios on purpose — re-detecting a recovered edge from zero effort is partly a re-exploration draw):

```text
[Phase 1: Responsive] ──▶ [Phase 2: Flat / No Benefit] ──▶ [Phase 3: Partial Benefit]
```

* **Phase 1 → Phase 2:** When boosting frequencies no longer reduces frame delay, the engine automatically drops effort toward zero.
* **Phase 2 → Phase 3:** When responsiveness partially returns, the engine's surprise-detection mechanism re-opens exploration, settling on a conservative operating point.

---

## 3. Real-World Kernel eBPF Runqueue Latency (Galaxy M54 120 Hz Display)

Measured on physical hardware (Samsung Galaxy M54 5G, Exynos 1380, 120 Hz Super AMOLED Plus panel, Kernel 5.15 GKI) via `module/bin/runqueue.bpf.o` attached to `sched/sched_switch`:

| Metric | Measured Range | Impact on 120 Hz Frame Budget (8.33 ms) | Impact on 60 Hz / 30 FPS Budgets |
|---|---|---|---|
| **Monitored Foreground Threads** | 64 threads | Targeted monitoring; ignores idle background tasks | Same |
| **Mean Scheduler Runqueue Latency** | **0.16 ms – 0.35 ms** | Consumes only 2%–4% of the 8.33 ms window | Negligible |
| **Peak Scheduler Runqueue Latency** | **7.0 ms – 11.9 ms** | **Consumes 84% to 143% of the entire frame budget!** Guarantees an immediate dropped frame / stutter at 120 Hz. | Consumes 42%–71% of a 60 Hz budget (16.6 ms) |
| **Instances Exceeding 4.0 ms** | **0.5% – 1.4%** | Consumes >48% of the 120 Hz window before rendering begins | Noticeable pacing jitter |

On a 120 Hz panel, the frame window is razor-thin: **8.33 ms**. Legacy tools (such as `/proc/stat` or standard `dumpsys SurfaceFlinger`) only show average utilization or post-facto frame intervals; they are completely blind to thread scheduling delays. The in-kernel eBPF probe measures that delay — e.g. a render thread sitting runnable for 11.9 ms in the Linux scheduler queue directly explains 120 Hz frame drops even with GPU load below 80%.

What the probe does NOT do today is steer actuation: the queue channel is carried into the
feature vector and the history file as telemetry, but it is deliberately excluded from the
objective (`Deficit::primary()` reads frames, else stall) until it demonstrates correlation with
observed stutter on this device's own windows. Any sentence of the form "the controller detects
queue pressure and raises floors" describes a future, not the current code.

---

## 4. Invariant Testing under AddressSanitizer & UndefinedBehaviorSanitizer

Compiled with Clang under `-fsanitize=address,undefined -fno-omit-frame-pointer`:

```text
=================================================================
engine: PUCT search, critic, policy prior, replay, allowance, backoff,
        persistence, telemetry, ownership and crash recovery passed.
=================================================================
>> 0 memory leaks, 0 buffer overflows, 0 undefined behaviors detected.
```
