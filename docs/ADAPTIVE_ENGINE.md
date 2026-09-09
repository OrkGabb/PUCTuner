# PUCTuner Adaptive Engine Architecture

Standalone native ARM64 daemon (`bin/m54-adaptive`), zero external runtime dependencies (no Python, no Termux, no network, no TensorFlow Lite). The Android APK configures and monitors; closing the app never kills the daemon.

This document outlines the measured physical behavior on device versus model assumptions.

---

## Definition of Workload & Success

The engine is workload-agnostic: applications enter the context as cryptographic hashes, never categorized as "game vs non-game". The daemon has demonstrated rapid online adaptation across eight distinct apps within hours.

Rather than assuming fixed heuristics, success is quantified as **unmet service demand versus systemic cost**:

### Service Deficit Channels

| Channel | Measured Signal | Valid Scope | Reward Inclusion |
|---|---|---|---|
| `frame` | Presentation interval $p95$ vs target refresh rate | Active rendering | Yes (highest priority) |
| `queue` | Thread wait time in Linux scheduler runqueue via eBPF | When probe is attached | Telemetry / feature |
| `stall` | Kernel Pressure Stall Information (PSI) for CPU, memory, I/O | Always readable | Yes (fallback when no frame channel) |

"Unmeasured" and "measured zero" are strictly differentiated: missing data is flagged rather than fed as zero delay.

### Decoupled Acting and Learning

- **Acting** requires the device to be within thermal safety limits *and* in active rendering regime.
- **Learning** requires only valid sensor telemetry: readable sysfs nodes, valid deficit channel, and real workload demand.

When the engine is constrained from acting (e.g. thermal backoff), real state transitions under passive control still contribute a **value backup**.

---

## The Decision Engine: MCTS + PUCT

Instead of static rules, the core runs a rolling-horizon Monte Carlo Tree Search with PUCT selection:

$$\text{PUCT}(s, a) = Q(s, a) + c_{\text{puct}} \cdot P(s, a) \cdot \frac{\sqrt{\sum_b N(s, b)}}{1 + N(s, a)}$$

1. **State Space ($s$):** Discrete multidimensional tuple of CPU load tier, GPU load, MIF bus demand, thermal zone, and active app hash.
2. **Action Space ($a$):** Coordinated relative steps across 4 hardware axes:
   - CPU Little/Big cluster frequency floors
   - Mali-G68 GPU frequency floors
   - Memory bus (MIF) QoS frequency floors
   - PELT multiplier (1x vs 2x)
3. **Critic & Value Network:** Online Temporal-Difference learning (TD-learning) continuously updating value estimates $V(s)$.
4. **Dirichlet Noise & Exploration:** Root Dirichlet noise injection prevents premature convergence.
5. **Evidence Aging:** Prior visit counts decay over time ($2^{-\Delta \text{age} / 64}$), prompting periodic re-evaluation.
6. **Surprise Threshold:** When real cost exceeds 4 standard deviations from prediction, prior authority is reset to trigger instant re-exploration.
