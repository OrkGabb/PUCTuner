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
5. **Evidence Aging:** Two half-lives: prediction trust decays as $2^{-\Delta \text{age} / 1024}$,
   the novelty re-ask clock as $2^{-\Delta \text{age} / 64}$ (see `HOW_IT_WORKS.md` §7 — one
   constant served both jobs and destroyed the first).
6. **Surprise Threshold:** When real cost exceeds 4 standard deviations from prediction, prior authority is reset to trigger instant re-exploration.

## Reading Decision Telemetry

`adaptive_status` reports `axes_allowed` as four digits in CPU, GPU, MIF, PELT order.
`1111` permits all four axes. `refusals` lists their write-rejection counts in the same
order, separated by `/`; `refusal_limit` reports the threshold that holds an axis off.
Each count decays after fifteen quiet minutes, so a transient QoS storm re-probes
gradually instead of latching until restart. `base_allowed` is the stable capability
behind the context identity; `axes_allowed` is the transient permission the search used.
An axis can also be unavailable because of capability, configuration, ownership
constraints, or temporary backoff. A disabled axis alone does not identify which cause
applied. Known imperfection, deliberately left in: PELT levels 0 and 1 both write the 2x
baseline yet price differently. Both minimal corrections breach the furnace scenario, so
the fix waits on a device-measured PELT heat term in the transition model rather than on
a constant tuned until the synthetic device passes.

`adaptive_history.csv` records one row per measurement window. Read columns by name:

| Column | Meaning |
|---|---|
| `reason` | What the controller did this window, including settling, search, or external writes. |
| `gate` | First failed exploration condition: `learning_off`, `die_hot`, `battery_hot`, `charge_low`, or `allowance`; `open` permits exploration. `-` means search was not reached. |
| `want_move` | Best modeled costlier candidate among the permitted moves; `-1` means none was evaluated. This is not necessarily the planner's chosen move. |
| `want_adv` / `want_toll` | Predicted immediate reward advantage versus staying, and the acceptance threshold. |
| `want_tried` | Recorded observations for that candidate's edge in the current model context. Zero means the edge has not been measured there. |
| `want_ok` | Whether that candidate would pass acceptance now. It does not mean the planner selected it or the hardware applied it. |
| `axes` | Permission bitmask used for this window's search: CPU=1, GPU=2, MIF=4, PELT=8. `15` permits all axes. |

A closed exploration gate still permits a costlier move whose modeled advantage clears
the threshold. Conversely, `want_ok=1` with little spending does not prove an acceptance
failure: MCTS chooses using its search, and this column describes a separate candidate.
Use `gate` to distinguish windows that reached search before interpreting `want_*`.

A window whose pair spans a successful RAM trim is recorded with credit
`trim_skipped`: its p95 and paging changes belong to the release, not to the DVFS floor
held across it, so value, policy and residual all stay out and the next window re-arms
from the post-trim state.

These fields observe the controller without changing its decisions. Schema changes and
size limits rotate the history through three generations (`adaptive_history.csv.1`
through `.3`); copy all four files before an update or a long measurement if the older
session must survive.
