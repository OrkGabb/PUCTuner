#pragma once
#include <map>
#include <string>
#include <vector>

namespace m54 {

// Minimal BPF object loader and runqueue-delay probe.
//
// No libbpf: that would mean cross-compiling a shared library for Android and shipping it
// alongside a daemon that is otherwise one self-contained executable. CO-RE is not needed
// either, because tracepoint argument layout is a kernel ABI published in
// /sys/kernel/tracing/events/<group>/<event>/format, which this loader reads and verifies
// before attaching. If any check fails the probe simply stays off and every other telemetry
// source keeps working exactly as before.
class RunqueueProbe {
public:
    RunqueueProbe() = default;
    ~RunqueueProbe();
    RunqueueProbe(const RunqueueProbe&) = delete;
    RunqueueProbe& operator=(const RunqueueProbe&) = delete;

    // Loads `object`, creates its maps, verifies the tracepoint ABI and attaches. Returns
    // false and leaves the probe inert on any failure; reason() says which.
    bool start(const std::string& object);
    bool active() const { return !links.empty(); }
    const std::string& reason() const { return why; }

    // Publishes the thread ids worth measuring. Everything else leaves the kernel program
    // after one hash lookup, which is what makes attaching to sched_switch affordable.
    void watch(const std::vector<int>& tids);

    // Drains the accumulator and resets it. False when no watched thread was scheduled.
    bool sample(double& meanMs, double& peakMs, double& lateShare);

    // Thread ids of a package's process, for watch(). Empty when the package is not running.
    static std::vector<int> threadsOf(const std::string& package, size_t limit = 64);

private:
    int mapFor(const std::string& name) const;
    void shutdown();
    std::map<std::string, int> maps;
    std::vector<int> programs, links;
    std::vector<int> watching;
    std::string why = "not_started";
};

} // namespace m54
