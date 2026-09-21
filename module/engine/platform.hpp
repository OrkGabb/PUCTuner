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
// Two clocks, because they answer two different questions. `monotonic` stops while the device
// is suspended, which is what the loop's own pacing and the subprocess timeouts want -- a deep
// sleep must not expire a 1.5 s dumpsys. `boottime` keeps counting through suspend, which is
// what anything measuring how much of the world went by needs. Reading both and subtracting is
// the only way the daemon learns it was asleep at all: nothing else tells it.
double monotonic();
double boottime();
std::vector<std::string> globPaths(const std::string& pattern);
// `exitCode`, when asked for, distinguishes "ran and failed" from "ran and printed nothing",
// which the return value alone cannot: both are the empty string. -1 means it never ran.
std::string command(const std::vector<std::string>& args, int timeoutMs = 1500, int* exitCode = nullptr);
// Names a foreign tuner writing the same DVFS nodes, or empty. The caller stands down while it
// is non-empty: measuring beside a second writer trains the model on another process's writes.
// Which tuning surface the engine is learning on, hashed from the configuration keys that
// change how the device answers its own writes. `legacyIdentities` maps the hashes older
// layouts produced for this same surface onto the current one, so retiring a key or
// reshaping the hash never silently orphans a brain.
std::string configIdentity(const std::map<std::string, std::string>& cfg);
std::map<std::string, std::string> legacyIdentities(std::map<std::string, std::string> cfg,
                                                    const std::string& current);
// Loads `dir/adaptive_model` into `brain`. A brain that does not load is set aside as
// `adaptive_model.rejected.<time>`, never left where the first save would overwrite it. Returns
// empty when a brain loaded or none was stored; otherwise `<why>:<where it went>`, where `why` is
// `identity` for a brain from another firmware or kernel (every OTA does this) and `invalid` for
// one that cannot be read. The daemon exports it so a reset is visible instead of silent.
std::string loadBrain(const std::string& dir, const std::string& identity,
                      const std::map<std::string, std::string>& rehome, Brain& brain);
std::string processConflict();
// Tuning nodes that exist but are not writable. A node another module left at mode 0444 is a
// different failure from a node this kernel does not have, and reporting both as "absent"
// loses a capability without ever saying why. Observed on this device: both CPU governors and
// four thermal zone modes, chmod'd by a module that had already been disabled.
std::vector<std::string> lockedTuningNodes();
std::string foreground(const std::string& activityDump);
std::string chooseLayer(const std::string& dump, const std::string& app);
long availableMemoryKb();
void swapMemoryKb(long& totalKb, long& freeKb);
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
// Deliberately NOT triggered by swap occupancy: a full zram after the user opened many apps is the
// cache doing its job, and killing it turns every routine app open into a cold start that costs
// more CPU and battery than the refault it avoids.
bool automaticRamTrimDue(const std::map<std::string, std::string>& cfg, const Observation& s,
                         bool transition, bool benchmark, double sinceLastAttempt);
// Fires the trim and says only whether the request was accepted. How much it actually freed is
// deliberately NOT returned: teardown and reclaim are asynchronous, so the only honest answer
// comes from the next window's MemAvailable, and main.cpp is where that comparison belongs.
bool trimBackgroundMemory();
// A measurement window closes on evidence, not on the clock.
//
// Every learned label rests on the p95 of the window's frame intervals, and the repeatability of
// that statistic is a step function of how many intervals went into it. Measured across 456 pairs
// of consecutive windows holding the same app, the same action and the same cadence, with no
// suspend in either (one week of this device's own history, 2026-09-17):
//
//     frames  40- 80   n= 52   |dp95|/p95 med 0.491   |d deficit| med 0.0439
//     frames  80-150   n= 80   |dp95|/p95 med 0.342   |d deficit| med 0.0062
//     frames 150-250   n=212   |dp95|/p95 med 0.003   |d deficit| med 0.0011
//     frames 250-400   n= 80   |dp95|/p95 med 0.002   |d deficit| med 0.0004
//
// Two windows of one unchanged device state have to agree or the critic is being trained on a
// label that contradicts itself -- the same failure as the cadence random-walk of 2026-09-09,
// reaching it through the sample size instead of through the denominator. The cliff is at 150.
//
// A fixed six seconds misses it in both directions, because the frame count is cadence times
// COVERAGE and the coverage is the part that varies: the median window presented for only 56% of
// its span. 27% of rendering windows never reached 150 intervals and were learned from anyway,
// while a window that had 150 in four seconds spent the remaining two adding nothing. Simulated
// over the same history, closing on evidence leaves the mean span at 5.98 s -- the sample rate is
// unchanged -- and moves the share of rendering windows at or above the cliff from 73% to 89%,
// with the noisy 24-80 band falling from 9.3% to 1.4%.
//
// Both bounds are set by sound()'s pair gate rather than by the frame statistics: it rejects a
// pair closer together than 4 s or further apart than 30 s, so a window may never close before
// 4.5 s however much evidence it has, nor run past 12 s however little.
constexpr double WindowFloor = 4.5;
constexpr double WindowNominal = 6.;
constexpr double WindowCeiling = 12.;
constexpr size_t WindowFrames = 150;
// The frame channel exists at all from here up; below it finish() reports framesValid = false.
// Shared with FrameTracker::finish so the close rule and the validity rule cannot drift apart.
constexpr size_t MinimumFrames = 24;
// `frames` is what the still-open window has accumulated so far. Pure, so the rule is testable
// without a device: main.cpp owns the clock, this owns the decision.
bool windowComplete(double span, size_t frames);
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
    // Awake time, not `s.at`. Every counter these two divide -- vmstat's faults, a thread's
    // utime -- only advances while the CPU is running, so the honest divisor is the time the
    // CPU was running. Dividing an awake-only delta by a span that counted a suspend reports a
    // device that was paging hard as one that was idle, and marks the reading valid.
    double threadAt = 0;
    double lastTemp = 0, lastAt = 0;
    // /proc/vmstat counters are monotonic since boot, so only their difference across a closed
    // window means anything. Differenced at the window boundary, never at the 1 s poll.
    uint64_t prevMajorFaults = 0, prevSwapIn = 0, prevFileRefault = 0;
    double pagingAt = 0; // awake time, like threadAt above
    std::string app, layer;
    double contextAt = -100;
    bool awake = false; // cached power state between the 6 s context refreshes
    FrameTracker frames;
public:
    // `moduleRoot` locates runqueue.bpf.o. An empty path, a missing object or any refusal from
    // the kernel leaves the probe off and every other source working exactly as before.
    explicit Sampler(const std::string& moduleRoot = {});
    Observation read(int cap, bool finishWindow);
    // Frame intervals the still-open window has accumulated. The caller decides whether to close
    // BEFORE it calls read(), so this is the count as of the previous 1 Hz poll: it lags by up to
    // one second of frames and is therefore a floor, never an overcount. A window closed on it
    // holds at least the frames it was asked for.
    size_t pendingFrames() const { return frames.samples.size(); }
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
    Constraints constrain(Constraints c, const std::map<std::string, std::string>& cfg) const;
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
