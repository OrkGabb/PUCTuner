// SPDX-License-Identifier: GPL-2.0
//
// Runqueue delay for the threads that actually draw the frame.
//
// This is the signal /proc cannot give. PSI reports aggregate stall across the whole device;
// what a stuttering frame needs is how long the render thread sat runnable before a CPU picked
// it up. It also keeps working when SurfaceFlinger exposes no layer for the foreground app,
// which is the blind spot that leaves the controller in `waiting_frames`.
//
// sched_switch fires tens of thousands of times per second on this SoC, so measuring every task
// would cost as much as the dumpsys polling it is meant to replace. Userspace publishes the
// handful of tids it cares about into `watched`; everything else leaves after one hash lookup.
//
// Field offsets are the kernel's tracepoint ABI, verified against
// /sys/kernel/tracing/events/sched/*/format on this device (5.15.189-android13-3). The loader
// re-checks them at runtime and refuses to attach if the kernel disagrees.

#define SEC(name) __attribute__((section(name), used))

typedef signed int __s32;
typedef unsigned int __u32;
typedef unsigned long long __u64;

#define BPF_MAP_TYPE_HASH 1
#define BPF_MAP_TYPE_ARRAY 2
#define BPF_ANY 0

struct bpf_map_def {
    __u32 type;
    __u32 key_size;
    __u32 value_size;
    __u32 max_entries;
    __u32 map_flags;
};

static void *(*bpf_map_lookup_elem)(void *map, const void *key) = (void *)1;
static long (*bpf_map_update_elem)(void *map, const void *key, const void *value,
                                   __u64 flags) = (void *)2;
static long (*bpf_map_delete_elem)(void *map, const void *key) = (void *)3;
static __u64 (*bpf_ktime_get_ns)(void) = (void *)5;

// tids userspace wants measured: the foreground app's threads plus the compositor.
struct bpf_map_def SEC("maps") m54_watched = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = 4,
    .value_size = 1,
    .max_entries = 512,
};
// tid -> timestamp at which it became runnable.
struct bpf_map_def SEC("maps") m54_waking = {
    .type = BPF_MAP_TYPE_HASH,
    .key_size = 4,
    .value_size = 8,
    .max_entries = 512,
};
// [0] total delay ns, [1] samples, [2] peak delay ns, [3] samples over 4 ms.
struct bpf_map_def SEC("maps") m54_delay = {
    .type = BPF_MAP_TYPE_ARRAY,
    .key_size = 4,
    .value_size = 8,
    .max_entries = 4,
};

struct wakeup_args {
    __u64 common;      // common_type/flags/preempt_count/pid
    char comm[16];     // offset 8
    __s32 pid;         // offset 24
    __s32 prio;        // offset 28
    __s32 target_cpu;  // offset 32
};

struct switch_args {
    __u64 common;
    char prev_comm[16];      // offset 8
    __s32 prev_pid;          // offset 24
    __s32 prev_prio;         // offset 28
    long long prev_state;    // offset 32
    char next_comm[16];      // offset 40
    __s32 next_pid;          // offset 56
    __s32 next_prio;         // offset 60
};

SEC("tracepoint/sched/sched_wakeup")
int m54_on_wakeup(struct wakeup_args *ctx) {
    __u32 tid = (__u32)ctx->pid;
    if (!bpf_map_lookup_elem(&m54_watched, &tid)) return 0;
    __u64 now = bpf_ktime_get_ns();
    bpf_map_update_elem(&m54_waking, &tid, &now, BPF_ANY);
    return 0;
}

SEC("tracepoint/sched/sched_switch")
int m54_on_switch(struct switch_args *ctx) {
    __u32 tid = (__u32)ctx->next_pid;
    __u64 *since = bpf_map_lookup_elem(&m54_waking, &tid);
    if (!since) return 0;
    __u64 now = bpf_ktime_get_ns();
    __u64 delay = now - *since;
    bpf_map_delete_elem(&m54_waking, &tid);
    // A wakeup that predates a suspend is not queue pressure; discard rather than average it in.
    if (delay > 1000000000ULL) return 0;

    __u32 slot = 0;
    __u64 *total = bpf_map_lookup_elem(&m54_delay, &slot);
    if (total) __sync_fetch_and_add(total, delay);
    slot = 1;
    __u64 *count = bpf_map_lookup_elem(&m54_delay, &slot);
    if (count) __sync_fetch_and_add(count, 1);
    slot = 2;
    __u64 *peak = bpf_map_lookup_elem(&m54_delay, &slot);
    // Racy against other CPUs by design: a lost peak update costs one window's worst case,
    // and a compare-exchange loop on every context switch does not.
    if (peak && delay > *peak) *peak = delay;
    slot = 3;
    __u64 *late = bpf_map_lookup_elem(&m54_delay, &slot);
    if (late && delay > 4000000ULL) __sync_fetch_and_add(late, 1);
    return 0;
}

char _license[] SEC("license") = "GPL";
