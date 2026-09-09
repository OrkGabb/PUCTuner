# eBPF/BTF Runqueue Telemetry in PUCTuner

Built and running in production. This document details the empirical measurements on hardware and the design tradeoffs of the in-kernel probe.

## Status: Real-Time Scheduler Monitoring

`module/engine/bpf/runqueue.bpf.c` measures the **runqueue delay** of the threads belonging to the active foreground application. It is loaded by `module/engine/bpf.cpp` (custom lightweight loader, zero dependency on external libbpf) and consumed continuously by the `Sampler`.

Live measurement during heavy 3D rendering:
```text
probe attached (ok)
threads=64   runqueue mean=0.16–0.35 ms   peak=7.0–11.9 ms   over_4ms=0.5–1.4%
```

With a 33.3 ms display budget at 30 FPS or 16.6 ms at 60 FPS, a peak wait of 11.9 ms spent merely waiting in the kernel runqueue consumes a massive portion of the frame budget before rendering even begins. Legacy sources—PSI, `/proc/stat`, or `dumpsys`—are completely blind to this latency.

The daemon exposes `queue_ms`, `queue_peak_ms`, `queue_late`, and `queue_valid` in its status file, `adaptive_history.csv`, and `adaptive_dataset.csv`.

## Why Custom Loader (Zero Libbpf Dependency)

This is a conscious design choice:
1. The loader **only consumes stable tracepoint arguments**, which follow published kernel ABI in `/sys/kernel/tracing/events/*/format`.
2. Offsets are verified byte-by-byte at runtime before attaching; the probe safely refuses to load if the kernel ABI diverges.
3. CO-RE exists to inspect internal kernel struct layouts (`task_struct`), which this probe deliberately avoids.
4. In exchange, the daemon remains a 100% self-contained native ARM64 binary: linking libbpf would require cross-compiling libelf and zlib, adding bloat versus ~9 KB for our self-contained loader.

## Hardware Verification

Probe execution verified in the daemon's native context (`uid=0`, `u:r:ksu:s0`) via `tools/bpf_probe.c`:

```text
MAP_CREATE (BPF_MAP_TYPE_ARRAY)   fd=3   ok
PROG_LOAD  (BPF_PROG_TYPE_TRACEPOINT) fd=4  ok     verifier accepted
/sys/kernel/tracing/events/sched/sched_switch/id   readable
/sys/kernel/btf/vmlinux                            readable
```

* **Kernel:** `5.15.189-android13-3-33470412` (GKI).
* **vmlinux BTF:** Present at `/sys/kernel/btf/vmlinux` (6,020,408 bytes).
* **Kernel Config:**
  ```text
  CONFIG_BPF=y            CONFIG_BPF_SYSCALL=y     CONFIG_BPF_EVENTS=y
  CONFIG_BPF_JIT=y        CONFIG_HAVE_EBPF_JIT=y   CONFIG_DEBUG_INFO_BTF=y
  CONFIG_KPROBES=y        CONFIG_UPROBES=y         CONFIG_FTRACE=y
  net.core.bpf_jit_enable=1   net.core.bpf_jit_harden=0
  ```
