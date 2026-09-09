# PUCTuner — KernelSU / Magisk Privileged Executor

Executes all hardware adjustments **in privileged init context**. The Android app serves strictly as the user interface: it writes configuration to `/data/adb/m54tuner/config` and triggers the execution scripts.

```text
APP (UI) ──writes──▶ /data/adb/m54tuner/config ──reads──▶ MODULE (Executor)
                     /data/adb/m54tuner/result ◀──writes── (Verified read-back)
```

## Execution Guarantees

Every application is serialized with atomic locks and outputs to a dedicated result file. Concurrent touches or boot hooks never corrupt reports or race on the same hardware nodes.

1. **Read-Back Verification:**  
   No knob is assumed applied without read-back. Every adjustment is written, re-read, retried up to 3 times, and logged to `/data/adb/m54tuner/result`:
   ```text
   R|gpu.min|ok|650000|650000
   R|cpu.policy4.max|fail|2002000|2400000
   ```
   The UI renders verified states directly.

2. **Scoped Restarts:**  
   Properties and nodes only trigger the minimal necessary restart scope:

| Scope | Target | Impact |
|---|---|---|
| `live` | sysfs/procfs: CPU, GPU, MIF, I/O, PELT, thermals, VM, GOS | Zero restarts; instantaneous |
| `apps` | `debug.hwui.*`, `ro.hwui.*` (renderer, caches) | `cmd activity force-stop` on selected target packages |
| `systemui` | System UI rendering properties | Only SystemUI restarts |
| `sf` | `debug.renderengine.*`, SurfaceFlinger backend | `ctl.restart surfaceflinger` (brief screen flicker) |

The `sf` scope is **never** executed automatically outside of boot: `apply_render.sh` flags `/data/adb/m54tuner/pending_sf`, presenting an explicit confirmation prompt in the app.

## Script Catalog

| Script | Tier | Execution Timing |
|---|---|---|
| `post-fs-data.sh` | — | Early boot: factory state snapshot & boot props (`apply_render.sh --boot`) |
| `service.sh` | all | Late boot: render → memory → profile, in strict sequence |
| `scripts/apply_profile.sh` | live | Live profile switching, thermals, GPU, I/O, GOS |
| `scripts/apply_render.sh` | apps/sf | Renderer backend, HWUI caches, RenderEngine, 120 FPS latch |
| `scripts/apply_mem.sh` | mem | ZRAM compression algorithm + swappiness |
| `scripts/apply_dexopt.sh` | dexopt | `speed-profile` by default; `speed` explicit option |
| `scripts/adaptive_start.sh`| live | Launches background `m54-adaptive` daemon |
