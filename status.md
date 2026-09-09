# PUCTuner — Project Status, Verification & Architectural Roadmap

## Update 2026-09-09 — v0.9.0 / APK 1.6.0 — Transition to Fully Autonomous Controller

Resumed from Codex session `01a08571-5dc1-7b53-aad6-ff2674ce3150`. The user noted that requiring manual profile selection (Game vs Powersave) forced the search algorithm down an artificially narrow corridor instead of allowing it to autonomously infer demand.

**Architecture & Reward Formulation:**
- **Profiles Were Never the Reward:** The reward function in PUCTuner is strictly exogenous and physical: multi-channel cost decomposition based on real kernel and GPU measurements (frame delay $p95$, jank, power/energy proxy, PSI stall pressure, SoC/battery thermals, safety boundary breaches, and intervention effort). Earlier profiles were merely static weight sets.
- **Production Objective Unification:** In `core.hpp`, `RuntimeObjective = Tier::Balanced` is permanently anchored. The engine operates as a truly autonomous agent. It requires zero manual hints that a game has launched: upon detecting frame lateness peaks, eBPF runqueue delays, or thread saturation, MCTS PUCT naturally prioritizes frame delivery by elevating hardware floors. When the demand subsides, energy and intervention penalties drive clocks back to baseline.
- **Evidence Aging (`freshEvidence`) and Surprise Detection:** In dynamic mobile workloads, stale transition estimates led to overconfidence and search stagnation. Exponential decay for older evidence was implemented ($N(s, a) \leftarrow N(s, a) \cdot 2^{-\Delta \text{age} / 64}$) alongside surprise detection (`surprises`): if real measurements deviate by more than $4\sigma$ from predictions, node authority in that context resets to 1 visit, prompting immediate re-exploration.

**Interface (APK 1.6.0 / versionCode 10):**
- Former "Profile" tab redesigned as **"Engine"**, featuring autonomous learning lifecycle controls, target frame rate, and real-time telemetry.
- PELT multiplier experimental benchmarks and diagnostic pausing moved to **"Lab"**.
- Full internationalization (PT/EN) parity across all UI strings in `Localization.kt`.

**Verification:**
- Closed-loop simulation `tests/engine_sim.cpp` passed: convergence, useless waste rejection, thermal ceiling respect, and dynamic adaptation to unannounced workload shifts (`engine_sim: automatic adaptation to unannounced workload changes passed`).
- AddressSanitizer/UBSan test suite `tests/engine_test.cpp` passed 100%.
- Native ARM64 cross-compilation (`m54-adaptive`) and module packaging (`m54tuner-module.zip`) complete.
- Android Release APK built with R8 shrinking (`m54tuner-release.apk`) verified.

---

## Update 2026-09-09 — APK 1.5.2 — Surgical Polish & Control Isolation

Resumed from Claude session `44b27490-e5c7-4570-985d-fa1d1aa1e93a`. Implemented verified UI grouping and control isolation:
- Grouping priority / automatic control, read-only hardware telemetry, manual controls, and laboratory experiments.
- Markers on controls that affect context hash. Modifications remain pending until confirmed; canceling writes zero changes.
- Confirmation modal displays targeted parameter explicitly (e.g. `Target FPS: 90`).
- Samsung SPCM/MARs hooks protected with explanatory warnings.

---

## Update 2026-09-09 — v0.8.1 / APK 1.5.1 — UI Decoupling & Read-Back Verification

Audit of user-reported placebo levers:
- **Zero Placebos in Module:** `apply_render.sh` never writes unverified props. All writes to sysfs undergo strict read-back verification; clamped writes are surfaced as `warn`/`CLAMPED` in `/data/adb/m54tuner/result`.
- **Removed 18 Dead Manual Overrides:** Retired conflicting manual knobs (`gpu_min`, `gpu_max`, `mif_min`) that previously conflicted with the autonomous engine.
- **9 Valid Context Knobs Retained:** Target FPS, adaptive ceiling, thermal mode, companion fas-rs, GOS, `fps_unlock`, and Samsung platform hooks.

---

## Update 2026-09-09 — v0.8.0 / APK 1.5.0 — Decoupling Profiles from Physics

**Multi-Component Critic:**
The scalar reward is linear in independent physical terms:
$$r = 1 - 2 \cdot (\mathbf{w} \cdot [\text{late}, \text{jank}, \text{energy}, \text{pressure}, \text{heat}, \text{rising}, \text{battery}, \text{breach}, \text{effort}])$$
The Critic predicts the discounted sum of each cost term individually. A single observed window trains all objective evaluations simultaneously.

**Physics Decoupled from Policy:**
Transition dynamics are shared globally. How a GPU floor step alters frame delay does not depend on user policy preferences.

**Context Identity Reduced from 52 to 27 Keys:**
Removed static properties (zram, ART, dexopt) from the context key, eliminating state fragmentation.

---

## Update 2026-09-08 — v0.7.0 / APK 1.4.0 — Workload-Agnostic Engine

The engine is workload-agnostic: applications enter the context as cryptographic hashes, never categorized as "game vs non-game".

**Multi-Channel Service Deficit:**
Rather than defining work solely by presented frames, service deficit is tracked across three distinct channels:
1. `frame`: Presentation interval $p95$ vs target refresh rate (active during rendering).
2. `queue`: eBPF thread runqueue latency tracking.
3. `stall`: Kernel PSI pressure stall information (active even without surface frame pacing).

---

## Hardware Reference: Samsung Exynos 1380 (`s5e8835`)

- **CPU Clusters:** 4× Cortex-A78 (`policy4`, 533–2400 MHz) + 4× Cortex-A55 (`policy0`, 533–2002 MHz).
- **GPU:** ARM Mali-G68 MP5 (Valhall 2nd Gen, DDK r38p1-01) controlled via `/sys/kernel/gpu/`.
- **Memory Bus (MIF):** Physical QoS ceiling at 2093 MHz.
- **eBPF Telemetry:** Kernel 5.15 GKI with BTF enabled, custom 9 KB loader, zero libbpf dependencies.

---

## Audit of Legacy `VideoPlayback_Vulkan` Module

1. `debug.renderengine.backend=skiaglvk` fails on modern Samsung SurfaceFlinger ("Non-threaded RenderEngine not supported"). Correct backend: `skiavkthreaded`.
2. `ro.hwui.*_cache` properties were deprecated and removed in modern Android.
3. Disabling Samsung SBWC increases DRAM bandwidth by up to 40%, harming efficiency. PUCTuner preserves SBWC.
