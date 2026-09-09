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
    std::vector<std::string> thermals;
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
