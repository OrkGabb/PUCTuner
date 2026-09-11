#pragma once
#include "bpf.hpp"
#include "core.hpp"
#include <set>

namespace m54 {
std::string readText(const std::string& path, size_t limit = 1024 * 1024);
bool writeText(const std::string& path, const std::string& text);
bool atomicText(const std::string& path, const std::string& text);
std::map<std::string, std::string> readConfig(const std::string& path);
double number(const std::string& text, double fallback = -1);
double monotonic();
std::vector<std::string> globPaths(const std::string& pattern);
// `exitCode`, when asked for, distinguishes "ran and failed" from "ran and printed nothing",
// which the return value alone cannot: both are the empty string. -1 means it never ran.
std::string command(const std::vector<std::string>& args, int timeoutMs = 1500, int* exitCode = nullptr);
std::string processConflict(bool& fas);
// Tuning nodes that exist but are not writable. A node another module left at mode 0444 is a
// different failure from a node this kernel does not have, and reporting both as "absent"
// loses a capability without ever saying why. Observed on this device: both CPU governors and
// four thermal zone modes, chmod'd by a module that had already been disabled.
std::vector<std::string> lockedTuningNodes();
std::string foreground(const std::string& activityDump);
std::string chooseLayer(const std::string& dump, const std::string& app);
long availableMemoryKb();
// Stable capability versus transient permission. `constrain()` reports which axes the
// device and configuration allow at all; short write rejections must never rewrite that
// answer, or the context identity fragments across masks. Keep both: the stable mask feeds
// `context()`, the transient one feeds the search and the actuator.
struct RefusalState {
    std::array<int, Axes> count{};
    std::array<double, Axes> blockedUntil{};
    std::array<double, Axes> lastChange{};
};
constexpr int MaxRefusals = 5;
constexpr double RefusalBackoffSec = 120;
// One count decays after this long without a new rejection, so a transient QoS storm
// re-probes gradually instead of latching an axis off until the daemon restarts.
// Provisional; fit from `refusals` telemetry rather than from intuition.
constexpr double RefusalDecaySec = 900;
bool axisAvailable(const RefusalState& state, int axis, double tick);
void axisRejected(RefusalState& state, int axis, double tick);
void decayRefusals(RefusalState& state, double tick);
Constraints withTransient(Constraints base, const RefusalState& state, double tick);
// Pending-trim state machine. `arm()` records MemAvailable when a trim is requested;
// `closeWindow()` runs once per closed window and reports whether the pair ending here
// spans that release. The arming is consumed exactly once, so one trim skips one window,
// whether or not the freed amount was readable — an unreadable window is still spanned,
// it just stays "unmeasured" instead of reporting a delta over several windows.
struct TrimGate {
    long baselineKb = 0;
    long freedKb = 0;
    bool measured = false;
    void arm(long memAvailKb) { baselineKb = memAvailKb; measured = false; freedKb = 0; }
    bool armed() const { return baselineKb > 0; }
    bool closeWindow(long memAvailKb);
};
// Builds the learning identity from the stable capability, never from the transient
// permission. A named step rather than a bare `context()` call so the call site states
// which mask it keys on; the contract (key follows base, ignores backoff) is pinned by
// tests, and any return to keying on the transient mask is a visible call-site change.
inline ContextKey stableKey(const Observation& s, const Constraints& base,
                            const std::string& configId) {
    return context(s, base, configId);
}
// Shift `path` to `path.1`, `.1` to `.2`, and so on up to `keep` generations, deleting the
// oldest. Repairing history by overwriting a single `.1` capped the evidence available for
// long-session investigations to whatever happened to survive last.
void rotateGenerations(const std::string& path, int keep = 3);
// Automatic process termination is a separate, explicit opt-in from the one-shot Game action.
bool automaticRamTrimDue(const std::map<std::string, std::string>& cfg, const Observation& s,
                         bool transition, bool benchmark, double sinceLastAttempt);
// Fires the trim and says only whether the request was accepted. How much it actually freed is
// deliberately NOT returned: teardown and reclaim are asynchronous, so the only honest answer
// comes from the next window's MemAvailable, and main.cpp is where that comparison belongs.
bool trimBackgroundMemory();
struct FrameTracker {
    int64_t last = 0;
    int64_t first = 0;
    double period = 0;   // display vsync period in ns, from the --latency header
    int cadence = 0;     // fps this app has demonstrated it can render at, sticky per app
    int slower = 0;      // consecutive windows the app failed to reach that cadence
    int faster = 0;      // consecutive windows it proposed a quicker one
    std::vector<double> samples;
    void addLatency(const std::string& dump, double now);
    // `cap` is the user's ceiling, not an assumption about the app: the cadence is measured.
    void finish(Observation& s, int cap);
    void reset() { last = first = 0; cadence = 0; slower = faster = 0; samples.clear(); }
};
class Sampler {
    struct Zone { std::string path, name; };
    std::vector<Zone> thermals;
    RunqueueProbe queue;
    double watchedAt = -100;
    uint64_t prevTotal = 0, prevIdle = 0;
    struct CoreTicks { uint64_t total = 0, idle = 0; };
    std::array<CoreTicks, 16> perCore{};
    std::map<int, uint64_t> threadTicks;
    double threadAt = 0;
    double lastTemp = 0, lastAt = 0;
    // /proc/vmstat counters are monotonic since boot, so only their difference across a closed
    // window means anything. Differenced at the window boundary, never at the 1 s poll.
    uint64_t prevMajorFaults = 0, prevSwapIn = 0, prevFileRefault = 0;
    double pagingAt = 0;
    std::string app, layer;
    double contextAt = -100;
    bool awake = false; // cached power state between the 6 s context refreshes
    FrameTracker frames;
public:
    // `moduleRoot` locates runqueue.bpf.o. An empty path, a missing object or any refusal from
    // the kernel leaves the probe off and every other source working exactly as before.
    explicit Sampler(const std::string& moduleRoot = {});
    Observation read(int cap, bool finishWindow);
    bool probeActive() const { return queue.active(); }
    const std::string& probeReason() const { return queue.reason(); }
};
struct Node {
    std::string path, maxPath, tablePath;
    int axis;
    long original = 0;
    long maxOriginal = 0;
    std::vector<long> table{};
};
class Actuator {
    struct Entry { long original, last; };
    std::string dir, boot;
    std::map<std::string, Entry> journal;
    std::vector<Node> nodes;
    std::vector<std::string> lockedPaths;
    int failureAxis = -1;
    bool saveJournal();
    bool writeOwned(const std::string& path, long value);
public:
    explicit Actuator(const std::string& dataDir, std::vector<Node> testNodes = {});
    Constraints constrain(Constraints c, const std::map<std::string, std::string>& cfg, bool fas) const;
    bool apply(Action action, const Constraints& limits);
    bool restore();
    bool verified() const;
    size_t capabilities() const { return nodes.size(); }
    const std::vector<std::string>& locked() const { return lockedPaths; }
    int rejectedAxis() const { return failureAxis; }
};
// Uses the same lock and PID/start-time protocol as scripts/lib.sh.
class ApplyLock {
    std::string path;
    bool owned = false;
public:
    explicit ApplyLock(const std::string& dir);
    ~ApplyLock();
    explicit operator bool() const { return owned; }
};
} // namespace m54
