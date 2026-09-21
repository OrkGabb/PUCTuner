# PUCTuner — Technical Architecture & Forensic Deep-Dive

This document provides an exhaustive, code-referenced explanation of PUCTuner's internal algorithms, mathematical models, kernel interactions, and safety guarantees.

---

## 1. Daemon Architecture & The 6-Second Rolling Horizon

### Where is it implemented?
* Header: [`module/engine/core.hpp`](../module/engine/core.hpp)
* Core Logic: [`module/engine/core.cpp`](../module/engine/core.cpp)
* Main Loop: [`module/engine/main.cpp`](../module/engine/main.cpp)
* Hardware Bridge: [`module/engine/platform.hpp`](../module/engine/platform.hpp), [`module/engine/platform.cpp`](../module/engine/platform.cpp)

### How does the control loop work?
The standalone ARM64 daemon (`bin/m54-adaptive`) polls sensors at 1 Hz and closes a measurement
window on frame evidence, not on the clock (`WindowFloor = 4.5 s`, nominal `6 s`, ceiling `12 s`,
`WindowFrames = 150`; see `windowComplete()` in `platform.hpp`). It runs without external runtimes
(no Python, no TensorFlow Lite, no network). Each closed window executes a discrete 6-stage pipeline:

```text
[1. Sensor Sampling] ──▶ [2. Model Observation] ──▶ [3. Critic TD-Update]
         │                                                    │
         ▼                                                    ▼
[6. Sysfs Actuation] ◀── [5. Acceptance Gate]   ◀── [4. MCTS PUCT Search]
```

1. **`Sampler::read()`**: Collects SurfaceFlinger presentation timestamps, eBPF scheduler runqueue delays, sysfs load/thermal states, and Linux kernel PSI (Pressure Stall Information).
2. **`Model::observe()`**: Computes the error between predicted metrics and empirical measurements from the previous window, updating residual variance.
3. **`Critic::learn()`**: Calculates Temporal-Difference (TD) error across all 9 cost dimensions simultaneously, adjusting linear value network weights bounded by `MaxStep = 0.04`.
4. **`Planner::search()`**: Executes Monte Carlo Tree Search with PUCT selection and truncated rollout bootstrapped by the Critic.
5. **`accept()`**: Verifies that the proposed DVFS adjustment clears an asymmetric profit toll, discarding micro-oscillations.
6. **`Actuator::apply()`**: Atomically logs the target state to `adaptive_journal` and commits frequency floors via sysfs with immediate read-back verification.

---

## 2. Invariant Unit Testing under ASan/UBSan

### Where is it implemented?
* Test Suite: [`tests/engine_test.cpp`](../tests/engine_test.cpp)
* Build Script: [`tools/build_engine.sh`](../tools/build_engine.sh)

### What does it actually test?
`engine_test.cpp` contains ~1300 lines of deterministic assertions compiled with `-fsanitize=address,undefined`
(the exact count grows with the engine; line links below name symbols, not pinned lines, so they
survive refactors):

* **Fail-Safe Invariants (`safe()` in `core.cpp`):** Asserts that `safe()` unconditionally resets
  all floors to zero when the screen is off, sensor validation fails, battery is below 10%, or
  temperature reaches the *configured* limit — `Constraints::high` (default 75 °C, production
  `adaptive_thermal_limit`, clamped to 60–84) and `batteryHigh` (default 43 °C). There is no
  hardcoded 80 °C / 44 °C trip point; the documented numbers were stale.
* **Thermal-Energy Allowance (`Budget`):** Verifies that continuous high effort on a hot device
  drains allowance headroom (collapsing permissible floors), while effort on a cold device incurs
  (near-)zero allowance penalty.
* **Critic Gradient Clamping:** Ensures no single transition moves any feature weight by more than 0.04, guaranteeing numerical stability.
* **Serialization & State Migration:** Verifies serialization round-trips (`M54_BRAIN_5`) and the
  `rehome` identity migration, including the retired-key case that once orphaned a brain silently
  (regression test teaches under an old identity and requires the new one to answer).

---

## 3. Closed-Loop Simulation with Unannounced Workload Shifts

### Where is it implemented?
* Simulation Engine: [`tests/engine_sim.cpp`](../tests/engine_sim.cpp)

### What is the purpose of the simulation?
As stated directly in the code comments:
> *"The synthetic device is deliberately simple, and passing here is NOT evidence of a gain on the real M54: it only shows the loop converges toward the objective it was given, refuses to spend where nothing is bought, and respects its thermal limit under a hostile heat curve."*

### Simulation Benchmark Protocol (static scenarios averaged over 7 random seeds)
The test subjects the controller to synthetic workloads with unannounced changes in device physics
(see `tests/engine_sim.cpp`; run it for the current numbers — point values below are NOT pinned,
they drift with seeds and toolchains, and a bar cleared by 0.05 at 7 seeds pins the draw, not the
behaviour):
1. **Responsive Workload:** Hardware floors actively reduce p95 frame delay. The controller finds a
   high-effort operating point with low p95 (asserted: `gpu >= 2`, `p95 < 16.7 ms`).
2. **Flat / Unresponsive Workload:** Increasing frequency floors produces zero reduction in frame delay (e.g. bottlenecked on I/O or game engine limits). The controller detects zero return, refusing to waste energy (asserted: `effort < 0.5`).
3. **Hostile Thermal Curve:** Environmental heat increases sharply. The controller throttles effort to
   stay below the ceiling with **zero safety breaches** (worst seed reported, not just the mean).
A separate phase test (21 seeds) checks adaptation to a benefit that disappears and later returns;
its pass bars are deliberately looser because re-detecting a recovered edge from zero effort is
partly a re-exploration draw.

---

## 4 & 5. PUCT Decision Algorithm, Q, P, N & Dirichlet Noise

### Where is it implemented?
* Search Engine: `Planner::search()` in `module/engine/core.cpp`

### Mathematical Formulation
During the selection phase, the tree descends by maximizing the PUCT score:

$$\text{PUCT}(s, a) = Q(s, a) + c_{\text{puct}} \cdot P(s, a) \cdot \frac{\sqrt{\sum_b N(s, b)}}{1 + N(s, a)}}$$

In C++ (`Planner::search()`; `parentVisits` is the square root of the parent's visit count):
```cpp
const double parentVisits = std::sqrt(std::max(1., double(tree[selected].visits)));
const double q = n.visits ? n.sum / n.visits / ValueLimit : 0;
const double score = q + 1.25 * n.prior * parentVisits / (1. + n.visits);
```

* **Q(s, a):** Normalized mean expected value accumulated across simulations.
* **P(s, a):** Softmax prior distribution learned by `Prior::distribution()` for the active context.
* **N(s, a):** Visit counter of the candidate action branch.
* **Root Dirichlet Noise (`Planner::search()`):**
  ```cpp
  std::gamma_distribution<double> gamma(.6, 1);
  for (auto& x : noise) { x = gamma(rng); total += x; }
  root.pendingPrior[i] = .75 * root.pendingPrior[i] + .25 * noise[i] / total;
  ```
  Exploration noise is injected **strictly at the root** of the search tree where real decisions affect hardware, preventing random jitter deep inside simulated rollouts.
* **Predictive Thermal Pruning:** If the internal transition model predicts that expanding a node would push temperature within 1 °C of the thermal ceiling (die) or 0.5 °C (battery), that branch is pruned immediately without wasting search iterations.
* **Critic-Bootstrapped Tail:** Rollouts simulate up to `horizon = 4` steps under prior distributions, bootstrapping the remainder using the Critic value:
  ```cpp
  value += discount * brain.critic.value(state, limits, action);
  ```

---

## 6. Exogenous Multi-Channel Reward Function & Weights

### Where is it implemented?
* Weights & Normalization: `preference()` and `costs()` in `module/engine/core.cpp`

### Reward Formulation
Reward is linear in independent physical terms, scaled to [-1, 1]:

$$r(s, a) = \text{clip}\left(1 - 2 \sum_{i=0}^{8} w_i \cdot c_i, -1, 1\right)$$

### Objective Weight Profiles (w)
```cpp
//  Late   Jank   Energy Pressure Heat  Rising Battery Breach Effort
{{ .48,   .28,   .08,   .08,     .22,  .12,   .30,    1.0,   .005 }}, // Game
{{ .32,   .23,   .25,   .10,     .22,  .12,   .30,    1.0,   .025 }}, // Balanced
{{ .22,   .20,   .43,   .08,     .22,  .12,   .30,    1.0,   .040 }}, // Powersave
```
* **Game:** Heavily penalizes frame delay (0.48) and jank (0.28); minimal intervention toll (0.005).
* **Powersave:** Energy (0.43) and intervention toll (0.040) are prioritized; effort costs 8x more than in Game.
* **Universal Safety Limits:** Safety breaches (`BreachCost = 1.0`), battery limits (0.30), and thermal constraints (0.22) share identical weights across all profiles.

### Metric Normalization
* **`LateCost`:** Primary service deficit from frame pacing (or stall if headless).
* **`JankCost`:** Frame interval variance clip in [0, 1].
* **`EnergyCost`:** Operating power proxy clip in [0, 1.5].
* **`PressureCost`:** Kernel PSI formula: clip(CPU_psi + 1.5 * Mem_psi + IO_psi, 0, 1).
* **`HeatCost`:** Normalized excess above operating band: clip((T - (T_high - 12)) / 12, 0, 2).
* **`EffortCost`:** Hardware floor step index divided by `MaxEffort` (16).

---

## 7. Transition Dynamics, Evidence Aging & Surprise Triggers

### Where is it implemented?
* Model & Evidence: `Model::predict()` / `Model::observe()` in `module/engine/core.cpp`

### Exponential Evidence Decay
Unlike static tuners that treat old observations as eternal truth, PUCTuner applies half-life aging —
with TWO half-lives, because one constant was doing two jobs that want opposite answers:

$$N_{\text{fresh}} = N \cdot 2^{-\Delta \text{age} / 1024} \quad \text{(prediction trust)}$$
$$N_{\text{revisit}} = N \cdot 2^{-\Delta \text{age} / 64} \quad \text{(novelty budget)}$$

* Prediction asks "how much of this residual do I believe?" — long memory (`EvidenceHalfLife = 1024`).
  Running this on 64 destroyed a week of measurements: the median cell had decayed by 2^-27 and not
  one transition edge survived.
* The novelty budget asks "is it time to re-test this alternative?" — short clock
  (`RevisitHalfLife = 64`), deliberately forgetful, and asymmetric: releasing effort gets 8 tries,
  spending more gets 3, because releasing is the recoverable direction.

### Surprise-Triggered Re-Exploration
If real measured frame latency deviates from the model's mean prediction by more than 4 residual standard deviations:

$$|\text{actual} - \text{predicted}| > \max(4.0, 4 \cdot \sqrt{\sigma^2 + 1.0})$$

The engine detects that workload conditions (e.g. entering a complex combat scene) have broken prior assumptions. It **resets the authority of all cells in that context to 1 visit**, immediately triggering active re-exploration.

---

## 8. Anti-Oscillation & DVFS Chattering Prevention

### Where is it implemented?
* Gate: `accept()` in `module/engine/core.cpp`

### The Acceptance Toll
High-frequency DVFS switching (*chattering*) degrades performance due to PLL lock latencies and PMIC voltage settling (~100–500 µs). PUCTuner suppresses chattering through three independent mechanisms:

1. **Rollout Transition Penalty:** Any simulated move that changes the hardware state pays an immediate cost penalty:
   ```cpp
   child.immediate = reward(...) - (choice.action != parentAction ? .015 : 0);
   ```
2. **Asymmetric Acceptance Toll (`accept()`):**
   ```cpp
   const double toll = costlier ? (c.tier == Tier::Game ? .008 : .015) : .004;
   if (advantage >= toll) return true;
   ```
    Increasing frequency floors requires proving a significant expected advantage over the current state (0.015, or 0.008 for the Game price list). Conversely, releasing frequency floors requires only a small margin (0.004), ensuring the device returns to low-power baseline as soon as demand subsides.
3. **Exploration Cap:** Untried states with higher effort are limited to at most 3 exploratory attempts before requiring strict advantage proof; releasing effort gets 8, because coming back down is the recoverable direction.

---

## 9. Hardware Actuation & Crash-Resilient Journaling

### Where is it implemented?
* Actuator: `Actuator` in `module/engine/platform.cpp`

### Sysfs Nodes (Samsung Exynos 1380)
* **CPU:** `/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq`
* **GPU:** `/sys/kernel/gpu/gpu_min_clock` and `gpu_max_clock`
* **Memory Bus (MIF):** `/sys/class/devfreq/*mif*/min_freq`
* **Kernel Scheduler:** `/proc/sys/kernel/sched_pelt_multiplier`

### Safety Guarantees
* **Never Pin `min == max`:** The actuator caps floor adjustments at the second-highest frequency table step (`table[table.size() - 2]`), allowing kernel governors to scale up while preventing premature thermal throttling.
* **Write-Ahead Recovery Journal (`adaptive_journal`):** Before modifying any sysfs node, the original factory clock and the last targeted clock are written atomically to persistent storage. If `m54-adaptive` receives `SIGKILL`, the subsequent startup detects the journal and restores stock frequencies.
* **Read-Back Verification:** Every write is immediately read back from the kernel node. If the driver clamps or rejects the value, the write is aborted.

---

## 10. Multi-Tier Independent Thermal Fail-Safes

### Where is it implemented?
* Native Logic: `safe()` in `module/engine/core.cpp`
* Independent Daemon: [`module/scripts/thermal_guard.sh`](../module/scripts/thermal_guard.sh)

Thermal safety operates across completely isolated layers. All engine-side limits are the
*configured* ones (`adaptive_thermal_limit`, default 82 °C, clamped to 60–84; battery 43 °C) —
not hardcoded values:

```mermaid
flowchart TD
    A[Hardware Sensors] --> B[Layer 1: C++ MCTS Predictive Pruning]
    A --> C[Layer 2: C++ safe Cutoff Function]
    A --> D[Layer 3: Shell thermal_guard.sh Independent Daemon]
    B -->|Temp approaching high - 1| E[Prunes search branch]
    C -->|Temp >= configured high or Battery >= 43C| F[Zeroes all floors instantly]
    D -->|CPU >= 78C or Battery >= 45C| G[Force enables kernel thermal throttling]
```

1. **Layer 1 (Search Pruning):** MCTS refuses to simulate branches that heat the device beyond safety boundaries.
2. **Layer 2 (Engine Cutoff):** If sensor readings reach the configured SoC limit or 43 °C battery,
   `safe()` drops all hardware floors to Level 0 immediately. (The guard only runs while
   `thermal=aggressive`; in `moderate` the engine cutoff and the kernel's own throttling apply.)
3. **Layer 3 (Out-of-Process Fail-Safe):** `thermal_guard.sh` runs as an independent root process with its own PID. Even if the C++ daemon crashes or hangs, the shell guard monitors all thermal zones every 2 seconds. If CPU/GPU reaches 78°C or battery reaches 45°C, it immediately forces `/sys/class/thermal/thermal_zone*/mode` back to `enabled`.

---

## 11. Autonomous Memory Management & Scheduler Responsiveness (PELT)

### Where is it implemented?
* Scheduler Scaling: `Actuator::apply()` (PELT axis) in `module/engine/platform.cpp`
* Proactive RAM Manager: `automaticRamTrimDue()` in `module/engine/platform.cpp`, armed/measured in `module/engine/main.cpp`

### PELT Responsiveness Floor (2x Baseline)
* **Stock 1x Bottleneck:** Samsung Exynos stock kernel uses standard Linux PELT (`sched_pelt_multiplier = 1`), with a 32 ms halflife requiring ~64–96 ms before sudden load spikes trigger CPU frequency increases. This creates palpable micro-stutters during UI touch gestures and gaming frame pacing.
* **Guaranteed 2x Floor:** M54 Tuner enforces a permanent baseline of `sched_pelt_multiplier = 2` (16 ms halflife). Level 0 never resets to 1x.
* **Exploratory 4x Boost:** Under heavy render deficits, the autonomous engine can dynamically scale PELT to 4x (8 ms halflife) for instantaneous scheduler response.

### Background RAM Trimming

The profile-gated automatic clear is retired: static profiles are gone, so that condition could
never hold. Existing `game_ram_clear` keys are carried as inert compatibility tombstones because
older model identities hashed the whole config; new configs do not seed the key.
The two real mechanisms are the manual one-shot (`clear_ram.sh`, "Limpar agora" — its UI report is
clean only if MemAvailable actually grew) and the daemon's optional periodic trim, which
requires an explicit `adaptive_ram_management=1` in its configuration; an absent setting disables it. It runs only in active mode, during a
stable rendering window, outside benchmarks and foreground transitions. Memory below
1,500,000 kB or memory PSI above 0.08 makes a window eligible. Attempts are separated by
60 seconds, or 20 seconds below 600,000 kB, including failed attempts. App switches never
bypass this interval. Observe mode does not terminate processes.

`cmd activity kill-all` requests termination of background processes through ActivityManager.
It can increase later cold starts; it offers no guarantee of freed RAM or better frame pacing.
The request is synchronous with a bounded timeout; actual teardown and reclaim can complete
later. The daemon reports the net MemAvailable change at the following window, which also
includes concurrent allocations and can be negative.
