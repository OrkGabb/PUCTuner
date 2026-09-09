// Standalone trial for the runqueue probe. Kept out of the daemon on purpose: a new telemetry
// source has to be proven against the workload before it is allowed to influence a decision,
// and this can run beside a live session without touching the controller at all.
//
//   bpf_trial <package> [seconds]
#include "../module/engine/bpf.hpp"
#include "../module/engine/platform.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main(int argc, char** argv) {
    const std::string package = argc > 1 ? argv[1] : "";
    const int seconds = argc > 2 ? atoi(argv[2]) : 30;
    if (package.empty()) { fprintf(stderr, "usage: bpf_trial <package> [seconds]\n"); return 2; }
    if (getuid() != 0) { fprintf(stderr, "needs root\n"); return 2; }

    m54::RunqueueProbe probe;
    const std::string object = "/data/local/tmp/runqueue.bpf.o";
    if (!probe.start(object)) {
        fprintf(stderr, "probe did not start: %s\n", probe.reason().c_str());
        return 1;
    }
    printf("probe attached (%s)\n", probe.reason().c_str());
    for (int i = 0; i < seconds; ++i) {
        const auto tids = m54::RunqueueProbe::threadsOf(package);
        probe.watch(tids);
        sleep(1);
        double mean = 0, peak = 0, late = 0;
        if (probe.sample(mean, peak, late))
            printf("%2ds  threads=%2zu  runqueue mean=%6.3f ms  peak=%7.3f ms  over_4ms=%5.1f%%\n",
                   i + 1, tids.size(), mean, peak, late * 100);
        else
            printf("%2ds  threads=%2zu  no watched thread was scheduled\n", i + 1, tids.size());
        fflush(stdout);
    }
    return 0;
}
