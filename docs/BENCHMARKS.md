# PUCTuner — Benchmarks & Empirical Validation

This document records the empirical validation of PUCTuner across unit testing, closed-loop simulation, kernel eBPF scheduler telemetry, and hardware node verification.

---

## 1. Closed-Loop Simulation Benchmarks

Simulated across **900 windows of 6 seconds each (1.5 hours of continuous execution per workload)**, evaluated across **7 independent random seeds** with synthetic noise, latency jitter, and thermal accumulation.

### Scenario Results

| Scenario | Objective | Mean Effort (0–16) | Mean GPU Floor | Frame Latency p95 | Normalized Reward | Peak SoC Temp | Thermal Breaches (> 75°C) |
|---|---|---:|---:|---:|---:|---:|---:|
| **Game / Responsive** | Responsive | **3.10** | 2.58 | **15.1 ms** | **+0.844** | 47.5°C | **0** |
| **Game / Flat (Unresponsive)** | No scaling benefit | **0.37** | 0.10 | 26.0 ms | +0.550 | 44.6°C | **0** |
| **Game / Hostile Heat** | Heavy heat accumulation | **2.07** | 1.69 | 18.8 ms | +0.747 | 73.4°C | **0** |
| **Powersave / Responsive** | High energy penalty | **1.50** | 1.35 | 20.3 ms | +0.533 | 40.3°C | **0** |
| **Balanced / Responsive** | Production default | **1.90** | 1.78 | 18.5 ms | +0.698 | 42.2°C | **0** |

### Key Findings:
1. **Refusal to Overspend:** When the workload ceases to benefit from higher frequencies (`Game/Flat`), the controller **reduces effort by 88%** (from 3.10 to 0.37), refusing to burn energy for zero frame pacing gain.
2. **Thermal Ceiling Respect:** In the hostile heat curve scenario, the controller dynamically trims hardware floors, capping peak temperature at **73.4°C** with **zero threshold breaches** over 898 evaluated windows.

---

## 2. Dynamic Adaptation to Unannounced Workload Shifts

Evaluated in continuous execution where the underlying hardware responsiveness changes without notifying the controller:

```text
[Phase 1: Responsive] ──▶ [Phase 2: Flat / No Benefit] ──▶ [Phase 3: Partial Benefit]
     Effort: 1.90                 Effort: 0.17                    Effort: 0.73
     p95: 18.5 ms                 p95: 26.0 ms                    p95: 23.4 ms
     Reward: +0.698               Reward: +0.518                  Reward: +0.580
```

* **Phase 1 → Phase 2:** When boosting frequencies no longer reduces frame delay, the engine automatically **drops effort from 1.90 to 0.17 (-91%)**.
* **Phase 2 → Phase 3:** When responsiveness partially returns, the engine's surprise-detection mechanism re-opens exploration, settling on a conservative operating point (**effort 0.73**).

---

## 3. Real-World Kernel eBPF Runqueue Latency (Galaxy M54)

Measured on physical hardware (Samsung Galaxy M54 5G, Exynos 1380, Kernel 5.15 GKI) via `module/bin/runqueue.bpf.o` attached to `sched/sched_switch`:

| Metric | Measured Range | Impact on Frame Budget (33.3 ms @ 30 FPS / 16.6 ms @ 60 FPS) |
|---|---|---|
| **Monitored Foreground Threads** | 64 threads | Targeted monitoring; ignores idle background tasks |
| **Mean Scheduler Runqueue Latency** | **0.16 ms – 0.35 ms** | Normal queue latency under light load |
| **Peak Scheduler Runqueue Latency** | **7.0 ms – 11.9 ms** | Consumes up to **71%** of a 60 FPS frame budget in CPU queue wait |
| **Instances Exceeding 4.0 ms** | **0.5% – 1.4%** | Directly correlates with visible micro-stutters and frame drops |

Legacy tools (such as `/proc/stat` or `dumpsys SurfaceFlinger`) only show average utilization or completed frame times; they cannot determine whether a missed frame was caused by GPU shader complexity or by the render thread waiting 11.9 ms in the Linux scheduler runqueue.

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
