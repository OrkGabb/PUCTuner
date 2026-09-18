#include "platform.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <poll.h>
#include <sstream>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

namespace m54 {
static std::string trim(std::string s) {
    auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return {};
    return s.substr(begin, s.find_last_not_of(" \t\r\n") - begin + 1);
}
std::string readText(const std::string& path, size_t limit) {
    std::ifstream in(path, std::ios::binary);
    std::string data; char buffer[4096];
    while (in && data.size() < limit) {
        in.read(buffer, std::min(sizeof(buffer), limit - data.size()));
        data.append(buffer, static_cast<size_t>(in.gcount()));
    }
    return data;
}
bool writeText(const std::string& path, const std::string& text) {
    int fd = open(path.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    auto n = write(fd, text.data(), text.size());
    close(fd);
    return n == static_cast<ssize_t>(text.size());
}
bool atomicText(const std::string& path, const std::string& text) {
    auto temp = path + ".tmp." + std::to_string(getpid());
    int fd = open(temp.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t pos = 0;
    while (pos < text.size()) {
        auto n = write(fd, text.data() + pos, text.size() - pos);
        if (n <= 0) { close(fd); unlink(temp.c_str()); return false; }
        pos += n;
    }
    bool ok = fsync(fd) == 0;
    close(fd);
    if (!ok || rename(temp.c_str(), path.c_str()) != 0) { unlink(temp.c_str()); return false; }
    auto parent = path.substr(0, path.rfind('/'));
    fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd >= 0) { fsync(fd); close(fd); }
    return true;
}
std::map<std::string, std::string> readConfig(const std::string& path) {
    std::map<std::string, std::string> cfg;
    std::istringstream in(readText(path, 65536)); std::string line;
    while (std::getline(in, line)) {
        auto eq = line.find('=');
        if (eq != std::string::npos && !line.empty() && line[0] != '#')
            cfg[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
    return cfg;
}
double number(const std::string& text, double fallback) {
    char* end = nullptr;
    double v = strtod(text.c_str(), &end);
    return end != text.c_str() && std::isfinite(v) ? v : fallback;
}
double monotonic() {
    timespec ts{}; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
double boottime() {
    timespec ts{}; clock_gettime(CLOCK_BOOTTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
std::vector<std::string> globPaths(const std::string& pattern) {
    glob_t g{}; std::vector<std::string> out;
    if (glob(pattern.c_str(), 0, nullptr, &g) == 0)
        for (size_t i = 0; i < g.gl_pathc; ++i) out.emplace_back(g.gl_pathv[i]);
    globfree(&g); return out;
}
std::string command(const std::vector<std::string>& args, int timeoutMs, int* exitCode) {
    if (exitCode) *exitCode = -1;
    int pipes[2]; if (pipe2(pipes, O_CLOEXEC) != 0) return {};
    pid_t pid = fork();
    if (pid == 0) {
        dup2(pipes[1], STDOUT_FILENO);
        int null = open("/dev/null", O_RDWR);
        dup2(null, STDERR_FILENO); dup2(null, STDIN_FILENO);
        close(pipes[0]); close(pipes[1]); close(null);
        std::vector<char*> argv;
        for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
        argv.push_back(nullptr); execvp(argv[0], argv.data()); _exit(127);
    }
    close(pipes[1]);
    if (pid < 0) { close(pipes[0]); return {}; }
    fcntl(pipes[0], F_SETFL, O_NONBLOCK);
    std::string output; bool complete = false;
    const auto deadline = monotonic() + timeoutMs / 1000.;
    while (monotonic() < deadline && output.size() < 2 * 1024 * 1024) {
        pollfd p{pipes[0], POLLIN, 0}; poll(&p, 1, 25);
        char buffer[8192]; auto n = read(pipes[0], buffer, sizeof(buffer));
        if (n > 0) output.append(buffer, n);
        else if (n == 0) { complete = true; break; }
        else if (errno != EAGAIN && errno != EINTR) break;
    }
    close(pipes[0]);
    if (!complete) kill(pid, SIGKILL);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (exitCode && complete && WIFEXITED(status)) *exitCode = WEXITSTATUS(status);
    return complete && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? output : std::string{};
}
// The context identity must track the tuning surface the engine competes with, not every switch
// the app happens to own. Hashing the whole file gave observe and active different identities, so
// alternating the two -- the only way to compare them in a game where pointing the camera at the
// sky changes the frame rate -- would have started a fresh context every switch and learned
// nothing. The same mistake, larger: 52 keys reached this hash, so changing the zram algorithm,
// the dexopt mode or the hand-written game list orphaned every residual cell and every policy
// the device had measured. Worse, `profile` was one of them, which split the transition model by
// preference on top of the tier already doing it -- measured here as 552 of 730 cells that
// differed by nothing else.
//
// What belongs here is what changes how the device answers the engine's own writes: the DVFS
// surface, the limits it must respect, and who else owns a lever. Everything else -- renderer,
// ART, zram, protection lists, dexopt -- changes the workload, and the workload is already
// measured, window by window, in the feature vector. It is state, not identity.
const char* const IdentityKeys[] = {
    "adaptive_target_fps", "adaptive_thermal_limit", "thermal", "gos",
    "cpu_gov", "io_sched", "gpu_gov", "gpu_min", "gpu_max", "gpu_hs_load", "gpu_hs_clock",
    "gpu_hs_delay", "gpu_power_policy", "gpu_cl_boost", "gpu_dvfs_period", "gpu_polling_speed",
    "gpu_js_period", "mif_min", "int_min", "disp_min", "ufs_rpm_lvl", "f2fs_ipu", "fps_unlock",
    "samsung_perf", "samsung_spcm", "samsung_mars_off",
};
// Keys that were in the identity and are not any more, each with the position it occupied and
// the values it could have held. Removing a key changes the hash of every context the device has
// ever measured, which orphans the brain -- silently, since an orphaned identity simply finds no
// cells. That is the same loss the profile rehoming below exists to prevent, and it needs the
// same treatment rather than a comment saying the key is gone.
//
// `fasrs_companion` retired in v0.11.0 with the companion mode itself: the engine no longer
// yields axes to fas-rs, it stands down from it like any other foreign tuner, so the setting no
// longer describes anything about the surface being tuned.
const struct { size_t at; const char* key; const char* values[3]; } RetiredIdentityKeys[] = {
    {3, "fasrs_companion", {"auto", "off", ""}},
};
std::string identityOf(const std::map<std::string, std::string>& cfg,
                              const std::vector<std::string>& keys) {
    std::ostringstream out;
    // `adaptive_pelt` is deliberately absent: which axes are permitted already reaches the
    // context as the allowed-axis mask, and hashing it here would say the same thing twice while
    // orphaning everything each time it is toggled.
    for (const auto& key : keys) {
        const auto found = cfg.find(key);
        out << key << '=' << (found == cfg.end() ? std::string() : found->second) << '\n';
    }
    return std::to_string(hash(out.str()));
}
std::vector<std::string> identityKeys() {
    return {std::begin(IdentityKeys), std::end(IdentityKeys)};
}
std::string configIdentity(const std::map<std::string, std::string>& cfg) {
    return identityOf(cfg, identityKeys());
}
// Identities this exact configuration would have produced under the previous rule. `profile` was
// inside that hash, so the same static surface yielded a different identity per profile; every
// one of those is provably the same surface as the current one and is rehomed onto it. An
// identity that matches none of them came from a configuration that cannot be reconstructed, and
// its cells are dropped instead of being folded in under a label they did not earn.
std::map<std::string, std::string> legacyIdentities(std::map<std::string, std::string> cfg,
                                                           const std::string& current) {
    std::map<std::string, std::string> rehome;
    for (const char* profile : {"game", "balanced", "powersave", "none"}) {
        auto probe = cfg;
        probe["profile"] = profile;
        std::ostringstream out;
        for (const auto& [key, setting] : probe) {
            if (key == "adaptive_mode" || key == "adaptive_learning") continue;
            if (key == "adaptive_pelt" && (setting.empty() || setting == "1")) continue;
            out << key << '=' << setting << '\n';
        }
        rehome[std::to_string(hash(out.str()))] = current;
    }
    // ...and the identities this configuration produced while a now-retired key was still part
    // of the hash. The value the key actually held is not recorded anywhere, so every value it
    // could have taken is offered; they are all the same physical surface, which is the whole
    // reason the key was retired. One retirement at a time: a second one would need the cross
    // product, and a silent partial answer is worse than an assert the day that happens.
    static_assert(sizeof(RetiredIdentityKeys) / sizeof(RetiredIdentityKeys[0]) == 1,
                  "rehoming more than one retired key needs the cross product of their values");
    for (const auto& retired : RetiredIdentityKeys) {
        for (const char* value : retired.values) {
            auto keys = identityKeys();
            keys.insert(keys.begin() + static_cast<long>(retired.at), retired.key);
            auto probe = cfg;
            probe[retired.key] = value;
            rehome[identityOf(probe, keys)] = current;
        }
    }
    return rehome;
}
std::string processConflict() {
    std::string conflict;
    for (const auto& p : globPaths("/proc/[0-9]*/cmdline")) {
        auto text = readText(p, 8192);
        std::replace(text.begin(), text.end(), '\0', ' ');
        // The command line is whatever a process chose to call itself; the executable path is
        // not. Encore's daemon reports "encored daemon" with no path at all, so matching the
        // command line alone missed eight live instances of it writing the same DVFS nodes --
        // for three days after its module had been disabled, because disabling a module does
        // not kill processes it already started. Read the link too, and match on either.
        const auto begin = p.find('/', 1) + 1;
        const auto pid = p.substr(begin, p.find('/', begin) - begin);
        char link[4096]{};
        const auto length = readlink(("/proc/" + pid + "/exe").c_str(), link, sizeof(link) - 1);
        const std::string identity = text + ' ' + (length > 0 ? std::string(link, length) : "");
        // fas-rs is a conflict, not a companion. It was the one foreign tuner this engine did
        // NOT stand down for: it set a flag that yielded the CPU and GPU axes and then went on
        // learning, so every window it ran through produced a before/after pair whose CPU and
        // GPU floors moved because ANOTHER process moved them, credited to whatever this engine
        // had chosen. That is the poisoned attribution the encore episode was made of, built in
        // on purpose. Two frame-aware tuners on one device is a misconfiguration; refusing to
        // act and saying so in `reason` is worth more than half-working beside it.
        if (identity.find("fas-rs") != std::string::npos &&
            identity.find(" run") != std::string::npos) conflict = "fas-rs";
        if (identity.find("ProjectRaco") != std::string::npos ||
            identity.find("raco_service") != std::string::npos) conflict = "ProjectRaco";
        if (identity.find("/encore/") != std::string::npos ||
            identity.find("encored") != std::string::npos) conflict = "encore";
        if (identity.find("definitive/brain.py") != std::string::npos) conflict = "definitive_ai";
    }
    return conflict;
}
// access(W_OK) is useless for this question when running as root: it answers "may I write",
// and root normally may, so it returns success even for a file at mode 0444 -- and then the
// write fails anyway, because sysfs enforces the mode. Ask about the bits themselves.
static bool ownerWritable(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0 && (info.st_mode & S_IWUSR) != 0;
}
std::vector<std::string> lockedTuningNodes() {
    // Deliberately wider than the engine's own axes: the thermal guard and the profile scripts
    // write nodes the engine never touches, and a lock on those is just as invisible.
    static const std::vector<std::string> patterns{
        "/sys/devices/system/cpu/cpufreq/policy*/scaling_governor",
        "/sys/devices/system/cpu/cpufreq/policy*/scaling_min_freq",
        "/sys/devices/system/cpu/cpufreq/policy*/scaling_max_freq",
        "/sys/kernel/gpu/gpu_governor",
        "/sys/kernel/gpu/gpu_min_clock",
        "/sys/kernel/gpu/gpu_max_clock",
        "/sys/class/devfreq/*mif*/min_freq",
        "/sys/class/devfreq/*mif*/max_freq",
        "/sys/class/thermal/thermal_zone*/mode",
        "/proc/sys/kernel/sched_pelt_multiplier",
    };
    std::vector<std::string> locked;
    for (const auto& pattern : patterns)
        for (const auto& path : globPaths(pattern))
            if (access(path.c_str(), F_OK) == 0 && !ownerWritable(path))
                locked.push_back(path);
    return locked;
}
std::string foreground(const std::string& dump) {
    std::istringstream in(dump); std::string line;
    while (std::getline(in, line)) {
        if (line.find("topResumedActivity=") == std::string::npos && line.find("mResumedActivity") == std::string::npos && line.find("ResumedActivity:") == std::string::npos) continue;
        auto slash = line.find('/');
        if (slash == std::string::npos) continue;
        auto start = line.find_last_of(" {", slash);
        auto app = line.substr(start == std::string::npos ? 0 : start + 1, slash - start - 1);
        if (!app.empty() && app.find('.') != std::string::npos && app.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") == std::string::npos) return app;
    }
    return {};
}
std::string chooseLayer(const std::string& dump, const std::string& app) {
    if (app.empty()) return {};
    std::istringstream in(dump); std::string line, best; int bestScore = -1;
    while (std::getline(in, line)) {
        if (line.find(app) == std::string::npos) continue;
        if (line.find("RequestedLayerState{") == 0) {
            line = line.substr(20);
            auto end = line.find(" parentId=");
            if (end != std::string::npos) line.resize(end);
            else if (!line.empty() && line.back() == '}') line.pop_back();
        }
        int score = line.find("SurfaceView") != std::string::npos ? 3 : line.find("$_") != std::string::npos ? 2 : 0;
        if (line.find("BLAST") != std::string::npos) ++score;
        if (score > bestScore && score > 0) { best = trim(line); bestScore = score; }
    }
    return best;
}
bool windowComplete(double span, size_t frames) {
    // The ceiling is absolute: past it the pair gate would reject whatever was measured.
    if (span >= WindowCeiling) return true;
    // So is the floor, for the same reason from the other side.
    if (span < WindowFloor) return false;
    // The evidence is in. Holding the window open longer only widens the chance that the
    // workload moves underneath a measurement that is already repeatable.
    if (frames >= WindowFrames) return true;
    if (span < WindowNominal) return false;
    // Past the nominal span with a frame channel that exists but has not reached the cliff:
    // keep collecting. This is the 27% of rendering windows that a fixed six seconds handed to
    // the critic as noise. Below MinimumFrames there is no frame channel to complete, and every
    // other channel in the window is a rate that the nominal span already measures -- waiting
    // there would spend throughput to learn nothing.
    return frames < MinimumFrames;
}
void FrameTracker::addLatency(const std::string& dump, double now) {
    std::istringstream in(dump); std::string line;
    // The header is the compositor's vsync period in nanoseconds, not a frame. This M54 panel
    // reports 8333333 (120 Hz); assuming 60 here makes an 8.3 ms frame look like half a budget
    // to spare when it is in fact exactly on time.
    std::getline(in, line);
    const double header = number(line, 0);
    if (header >= 1e6 && header <= 5e7) period = header;
    std::vector<int64_t> stamps;
    while (std::getline(in, line)) {
        int64_t desired, present, ready;
        std::istringstream row(line);
        if (!(row >> desired >> present >> ready)) continue;
        if (present <= 0 || present >= INT64_MAX / 2 || present > now * 1e9 || present < (now - 3) * 1e9) continue;
        stamps.push_back(present);
    }
    std::sort(stamps.begin(), stamps.end());
    stamps.erase(std::unique(stamps.begin(), stamps.end()), stamps.end());
    for (auto stamp : stamps) {
        if (stamp <= last) continue;
        if (last > 0 && samples.size() < 2048) {
            if (first == 0) first = last;
            samples.push_back((stamp - last) / 1e6);
        }
        last = stamp;
    }
}
void FrameTracker::finish(Observation& s, int cap) {
    s.frames = static_cast<int>(samples.size()); s.framesValid = samples.size() >= MinimumFrames;
    s.frameStart = first / 1e9;
    s.frameEnd = first > 0 ? last / 1e9 : 0;
    s.frameTimeMs = 0;
    for (double interval : samples) s.frameTimeMs += interval;
    s.slowFrames50 = std::count_if(samples.begin(), samples.end(), [](double x) { return x > 50.; });
    if (s.framesValid) {
        std::sort(samples.begin(), samples.end());
        s.p95 = samples[static_cast<size_t>(std::ceil(samples.size() * .95)) - 1];
        // The app's intended cadence. Neither the median nor a low percentile survives real
        // frame streams: a low percentile catches the bursts a compositor emits when two
        // buffers land together and reads a 30 fps lobby as 120, while a median lets an app
        // dropping half its frames be re-read as a slower app that is doing fine.
        //
        // So ask the question directly: what is the FASTEST cadence this app actually delivers
        // enough of the window at to be its intent? Dropped frames land on multiples of the base
        // period, so the slow tail is ignored on its own. "Enough of the window" used to read
        // "enough of the intervals", and that was wrong -- see below, bursts are neither rare
        // nor cheap to clear when every interval counts the same regardless of its length.
        const double panel = period > 0 ? 1e9 / period : 60;
        // Share of the window's TIME spent at a rate, not share of intervals. Counting intervals
        // weights an 8 ms present exactly as heavily as a 91 ms one, so a burst of paired
        // presents -- which is what a GPU-bound app's triple-buffered stream looks like -- clears
        // a 15% count bar while occupying 2% of the window, and the fastest divisor wins on
        // almost nothing. Measured here on 2026-09-09: NTE presenting a steady 19-22 fps was read
        // as 24, 30, 40, 60 and 120 within one session, and the SAME p95 of 58.1 ms scored
        // deficit 0.131 under 24 and 1.000 under 120. A critic cannot learn from a label that
        // swings sevenfold on an unchanged device, and no amount of run time repairs it -- the
        // contradictory windows are the training data. Time share is the axis on which a burst is
        // small and a genuinely fast app is still large: an app alternating 8.3 ms and 25 ms
        // spends a quarter of its window at 120, while a 20 fps stream with the same count of
        // 8.3 ms bursts spends two per cent there.
        const double elapsed = s.frameTimeMs;
        auto timeShare = [&](double target) {
            double held = 0;
            for (double interval : samples)
                if (interval >= target * .75 && interval <= target * 1.25) held += interval;
            return elapsed > 0 ? held / elapsed : 0.;
        };
        int best = 0;
        for (int divisor = 1; divisor <= 5 && best == 0; ++divisor) {
            const double hz = panel / divisor;
            if (hz < 20 || hz > cap + .5) continue;
            if (timeShare(1000. / hz) >= .15) best = static_cast<int>(std::lround(hz));
        }
        if (best == 0) {
            // Nothing matched a panel divisor: fall back to the median, still bounded by the cap.
            const double median = samples[samples.size() / 2];
            best = std::max(20, std::min(cap, static_cast<int>(std::lround(1000. / std::max(1., median)))));
        }
        // The cadence is a property of the app, not of the current window. Re-deriving it every
        // window makes the target chase whatever the phone happens to be delivering, so a scene
        // that collapses from 30 fps to 17 is re-read as "a 17 fps app, on time" and the
        // controller never sees a reason to act. Raise on demonstrated capability; lower only
        // after the app has sustained the slower rate, so one bad scene cannot redefine intent.
        const int proposed = best;
        if (cadence > 0 && cadence <= cap) {
            if (best > cadence) {
                // Raising is evidence-gated too now, if less strictly than lowering. Adopting a
                // faster divisor from a single window turned ordinary estimator noise into target
                // changes: after the time-share fix removed the 120 misread, NTE still moved
                // between 20, 24, 30, 40 and 60 in 17 of 160 windows, and the deficit for one
                // physical state (18-22 fps) still spanned 5x. The adjacent divisors genuinely
                // overlap -- +/-25% around 41.7, 33.3 and 25 ms leaves no gap -- so which one
                // clears first is decided by jitter. Lowering waits ten windows because a bad
                // scene must not redefine intent; raising waits three, because a capability that
                // just appeared should still be noticed inside twenty seconds.
                if (++faster < 3) best = cadence; else { faster = 0; slower = 0; }
            }
            else if (best < cadence) {
                faster = 0;
                if (timeShare(1000. / cadence) >= .05 || ++slower < 10) best = cadence;
                else slower = 0;
            } else { slower = 0; faster = 0; }
        }
        cadence = best;
        s.target = best;
        s.cadenceSeen = proposed;
        s.jank = std::count_if(samples.begin(), samples.end(), [&](double x) { return x > 1500. / s.target; }) / double(samples.size());
    }
    samples.clear();
    first = 0;
}
// Three counters out of the ~200 lines of /proc/vmstat, read once per closed window. The file
// is a flat "name value" table. `workingset_refault_file` is the split name; kernels before the
// anon/file split export the total as `workingset_refault`, and either answers the same question.
static void pagingCounters(uint64_t& majorFaults, uint64_t& swapIn, uint64_t& fileRefault) {
    std::istringstream in(readText("/proc/vmstat", 32768));
    std::string key; uint64_t value = 0;
    while (in >> key >> value) {
        if (key == "pgmajfault") majorFaults = value;
        else if (key == "pswpin") swapIn = value;
        else if (key == "workingset_refault_file" || key == "workingset_refault") fileRefault = value;
    }
}
static double psi(const std::string& path) {
    auto data = readText(path, 1024); auto pos = data.find("avg10=");
    return pos == std::string::npos ? 0 : std::clamp(number(data.substr(pos + 6), 0) / 100, 0., 1.);
}
static long meminfoField(const std::string& text, const std::string& key) {
    auto pos = text.find(key);
    if (pos == std::string::npos) return -1;
    pos += key.size();
    while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t')) ++pos;
    long val = 0;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
        val = val * 10 + (text[pos] - '0');
        ++pos;
    }
    return val;
}
long availableMemoryKb() { return meminfoField(readText("/proc/meminfo", 4096), "MemAvailable:"); }
void swapMemoryKb(long& totalKb, long& freeKb) {
    const auto text = readText("/proc/meminfo", 4096);
    totalKb = meminfoField(text, "SwapTotal:");
    freeKb = meminfoField(text, "SwapFree:");
}
bool automaticRamTrimDue(const std::map<std::string, std::string>& cfg, const Observation& s,
                         bool transition, bool benchmark, double sinceLastAttempt) {
    const auto enabled = cfg.find("adaptive_ram_management");
    const auto mode = cfg.find("adaptive_mode");
    if (enabled == cfg.end() || enabled->second != "1" ||
        (mode != cfg.end() && mode->second != "active") || benchmark || transition ||
        !s.awake || s.app.empty() || !s.framesValid || s.memAvailKb <= 0) return false;
    // Never bypass the cooldown on an app switch: that made ordinary navigation kill cached
    // apps repeatedly. Missing configuration must not silently opt into process termination.
    const double cooldown = s.memAvailKb < 600000 ? 20. : 60.;
    return sinceLastAttempt >= cooldown && (s.memAvailKb < 1500000 || s.memPsi > .08);
}
bool axisAvailable(const RefusalState& state, int axis, double tick) {
    if (axis < 0 || axis >= Axes) return false;
    return state.count[axis] < MaxRefusals && tick >= state.blockedUntil[axis];
}
void axisRejected(RefusalState& state, int axis, double tick) {
    if (axis < 0 || axis >= Axes) return;
    state.blockedUntil[axis] = tick + RefusalBackoffSec;
    if (state.count[axis] < 1000) ++state.count[axis];
    state.lastChange[axis] = tick;
}
void decayRefusals(RefusalState& state, double tick) {
    for (int i = 0; i < Axes; ++i) {
        if (state.count[i] <= 0) continue;
        if (tick - state.lastChange[i] >= RefusalDecaySec) {
            --state.count[i];
            state.lastChange[i] = tick;
            if (state.count[i] <= 0) state.blockedUntil[i] = 0;
        }
    }
}
Constraints withTransient(Constraints base, const RefusalState& state, double tick) {
    for (int i = 0; i < Axes; ++i)
        base.allowed[i] = base.allowed[i] && axisAvailable(state, i, tick);
    return base;
}
bool TrimGate::closeWindow(long memAvailKb) {
    if (baselineKb <= 0) return false;
    if (memAvailKb > 0) {
        // Signed on purpose. Clamping at zero merges "the kill freed nothing" with "it freed
        // 200 MB and the foreground allocated 250 back inside the same window", and those
        // call for opposite conclusions about whether the trim is worth doing at all.
        freedKb = memAvailKb - baselineKb;
        measured = true;
    }
    baselineKb = 0;
    return true;
}
void rotateGenerations(const std::string& path, int keep) {
    if (keep < 1) return;
    ::unlink((path + "." + std::to_string(keep)).c_str());
    for (int i = keep - 1; i >= 1; --i)
        ::rename((path + "." + std::to_string(i)).c_str(),
                 (path + "." + std::to_string(i + 1)).c_str());
    ::rename(path.c_str(), (path + ".1").c_str());
}
bool trimBackgroundMemory() {
    // `command` rather than std::system: no shell between us and the binder call, a bounded
    // timeout instead of an open-ended block inside a six-second control loop, and stdio that
    // cannot reach the daemon's own descriptors. `am` is only kept as a fallback for images
    // where `cmd` is missing, and it is tried only when `cmd` actually reported failure --
    // running both unconditionally would kill the background twice.
    int status = -1;
    command({"cmd", "activity", "kill-all"}, 2000, &status);
    if (status == 0) return true;
    command({"am", "kill-all"}, 2000, &status);
    return status == 0;
}
Sampler::Sampler(const std::string& moduleRoot) {
    for (const auto& z : globPaths("/sys/class/thermal/thermal_zone*")) {
        auto type = trim(readText(z + "/type", 128));
        if (type == "BIG" || type == "LITTLE" || type == "G3D") thermals.push_back({z + "/temp", type});
    }
    if (!moduleRoot.empty()) queue.start(moduleRoot + "/bin/runqueue.bpf.o");
}
Observation Sampler::read(int cap, bool finishWindow) {
    // Boot time, not monotonic. Every consumer of this field measures how far apart two
    // observations are: the pair gate in sound(), the thermal trend, and the foreground, paging
    // and per-thread caches below. CLOCK_MONOTONIC stops in suspend, so a twenty-minute deep
    // sleep reached all of them as an ordinary six-second step -- the gate that exists to reject
    // a stale pair could not see the staleness, and every cache carried what it held before the
    // phone slept straight into the first window after the unlock. The eBPF side already
    // discarded wakeups that predate a suspend; the userspace side never had the clock to.
    Observation s; s.at = boottime();
    // ...and the awake clock alongside it, for the two rates below. `s.at` answers "how
    // much of the world went by"; these counters only tick while the CPU runs, so their
    // rates need "how long was the CPU running" instead. The two agree except across a
    // suspend, which is exactly where getting it wrong would be invisible.
    const double mono = monotonic();
    // Provisional until the window closes and the real cadence is measured from the frames.
    s.target = std::max(20, std::min(cap, frames.cadence > 0 ? frames.cadence : cap));
    auto stat = readText("/proc/stat", 4096);
    std::istringstream in(stat.substr(0, stat.find('\n'))); std::string tag;
    uint64_t total = 0, idle = 0, v = 0; in >> tag;
    for (int i = 0; i < 8 && in >> v; ++i) { total += v; if (i == 3 || i == 4) idle += v; }
    s.loadValid = prevTotal > 0 && total > prevTotal && idle >= prevIdle && idle - prevIdle <= total - prevTotal;
    if (s.loadValid) s.cpu = 1. - double(idle - prevIdle) / double(total - prevTotal);
    prevTotal = total; prevIdle = idle;
    // Busiest single core. A saturated thread is the most common bottleneck in a game and it is
    // invisible in the aggregate above: one core pinned at 100% of eight reads as 12.5% overall,
    // which looks to a controller like plenty of headroom on a device that has none left.
    {
        std::istringstream cores(stat);
        std::string line;
        std::getline(cores, line); // the aggregate, already accounted for
        size_t index = 0;
        double peak = 0;
        bool measured = false;
        while (std::getline(cores, line) && line.rfind("cpu", 0) == 0 && index < perCore.size()) {
            std::istringstream row(line);
            uint64_t coreTotal = 0, coreIdle = 0, field = 0;
            row >> tag;
            for (int i = 0; i < 8 && row >> field; ++i) {
                coreTotal += field;
                if (i == 3 || i == 4) coreIdle += field;
            }
            auto& seen = perCore[index];
            if (seen.total > 0 && coreTotal > seen.total && coreIdle >= seen.idle &&
                coreIdle - seen.idle <= coreTotal - seen.total) {
                peak = std::max(peak, 1. - double(coreIdle - seen.idle) / double(coreTotal - seen.total));
                measured = true;
            }
            seen.total = coreTotal; seen.idle = coreIdle;
            ++index;
        }
        if (measured) s.cpuPeak = std::clamp(peak, 0., 1.);
    }
    s.gpu = std::clamp(number(readText("/sys/kernel/gpu/gpu_busy", 64), 0) / 100, 0., 1.);
    s.cpuPsi = psi("/proc/pressure/cpu"); s.memPsi = psi("/proc/pressure/memory"); s.ioPsi = psi("/proc/pressure/io");
    s.memAvailKb = availableMemoryKb();
    swapMemoryKb(s.swapTotalKb, s.swapFreeKb);
    int readTemps = 0;
    for (const auto& [path, zone] : thermals) {
        double t = number(readText(path, 64)) / 1000.;
        // Which zone won matters: this number vetoes every action at the limit and halves the
        // reachable level three degrees below it, and BIG, LITTLE and G3D do not run together.
        // Taking the max of three and recording none of them made a thermal veto unauditable.
        if (t > 0 && t < 120) { if (t > s.temp) { s.temp = t; s.hotZone = zone; } ++readTemps; }
    }
    auto supply = readText("/sys/class/power_supply/battery/uevent", 8192);
    std::map<std::string, std::string> bat; std::istringstream bi(supply); std::string line;
    while (std::getline(bi, line)) { auto eq = line.find('='); if (eq != std::string::npos) bat[line.substr(0, eq)] = line.substr(eq + 1); }
    s.batteryTemp = number(bat["POWER_SUPPLY_TEMP"]) / 10.;
    s.battery = static_cast<int>(number(bat["POWER_SUPPLY_CAPACITY"], 0));
    s.charging = bat["POWER_SUPPLY_STATUS"] != "Discharging";
    s.thermalValid = readTemps == static_cast<int>(thermals.size()) && readTemps > 0 && s.batteryTemp > 0 && s.batteryTemp < 65;
    if (lastAt > 0 && s.thermalValid) s.trend = (s.temp - lastTemp) / std::max(1., s.at - lastAt);
    lastTemp = s.temp; lastAt = s.at;
    // Samsung sec-battery on this device reports current_now in mA (voltage remains uV).
    double current = number(bat["POWER_SUPPLY_CURRENT_NOW"]), voltage = number(bat["POWER_SUPPLY_VOLTAGE_NOW"]);
    s.watts = std::abs(current) * voltage / 1e9;
    s.powerValid = !s.charging && current != -1 && voltage > 3000000 && voltage < 5000000 && s.watts > .05 && s.watts < 25;
    double cpuClock = 0; int clusters = 0;
    for (const auto& p : globPaths("/sys/devices/system/cpu/cpufreq/policy*")) {
        double cur = number(readText(p + "/scaling_cur_freq", 64), 0);
        double max = number(readText(p + "/cpuinfo_max_freq", 64), 1);
        cpuClock += std::pow(std::clamp(cur / std::max(1., max), 0., 1.), 2); ++clusters;
    }
    double gpuClock = std::clamp(number(readText("/sys/kernel/gpu/gpu_clock", 64), 0) / 949000., 0., 1.);
    s.energy = s.powerValid ? s.watts / 8. : .65 * s.cpu * cpuClock / std::max(1, clusters) + .35 * s.gpu * gpuClock * gpuClock;
    const bool refresh = finishWindow || s.at - contextAt >= 6;
    if (refresh) {
        awake = command({"dumpsys", "power"}).find("mWakefulness=Awake") != std::string::npos;
    }
    s.app = app;
    s.awake = awake && !app.empty();
    if (!layer.empty() && s.awake)
        frames.addLatency(command({"dumpsys", "SurfaceFlinger", "--latency", layer}, 1000), monotonic());
    // Close the window against the app whose layer actually produced these frames, and only
    // then look for a new foreground. Detecting first and resetting the tracker discarded every
    // window in which the user changed apps — on a phone in real use, most of them.
    if (queue.active() && finishWindow)
        s.queueValid = queue.sample(s.queueMs, s.queuePeakMs, s.queueLate);
    if (finishWindow) {
        uint64_t majorFaults = prevMajorFaults, swapIn = prevSwapIn, fileRefault = prevFileRefault;
        pagingCounters(majorFaults, swapIn, fileRefault);
        const double elapsed = mono - pagingAt;
        // A counter that went backwards is a wrap or a reset, not negative paging. Such a window
        // is left unmeasured rather than reported, which is the same distinction the frame and
        // queue channels make between "no evidence" and "evidence of zero".
        if (pagingAt > 0 && elapsed >= 1 && elapsed <= 120 && majorFaults >= prevMajorFaults &&
            swapIn >= prevSwapIn && fileRefault >= prevFileRefault) {
            s.majorFaults = (majorFaults - prevMajorFaults) / elapsed;
            s.swapIn = (swapIn - prevSwapIn) / elapsed;
            s.fileRefault = (fileRefault - prevFileRefault) / elapsed;
            s.pagingValid = true;
        }
        prevMajorFaults = majorFaults; prevSwapIn = swapIn; prevFileRefault = fileRefault;
        pagingAt = mono;
    }
    if (finishWindow) frames.finish(s, cap);
    if (refresh) {
        std::string nextApp;
        static const std::vector<std::string> activityArgs{"dumpsys", "activity", "activities"};
        if (awake) nextApp = foreground(command(activityArgs));
        if (nextApp != app) { app = nextApp; frames.reset(); layer.clear(); }
        if (!app.empty()) {
            auto next = chooseLayer(command({"dumpsys", "SurfaceFlinger", "--list"}), app);
            if (next != layer) frames.reset();
            layer = next;
        }
        contextAt = s.at;
        // Republish the watch list only when the foreground may have changed: scanning /proc
        // for a package's threads every second would cost more than the probe saves. The same
        // list answers the question the aggregate and the per-core figures both miss -- is one
        // thread of this app saturating a core? -- so it is reused rather than gathered twice.
        if (s.at - watchedAt >= 6) {
            const auto tids = app.empty() ? std::vector<int>{} : RunqueueProbe::threadsOf(app);
            if (queue.active()) queue.watch(tids);
            const double elapsed = mono - threadAt;
            std::map<int, uint64_t> current;
            double busiest = 0;
            for (int tid : tids) {
                const auto text = readText("/proc/" + std::to_string(tid) + "/stat", 4096);
                const auto close = text.rfind(')');
                if (close == std::string::npos) continue;
                std::istringstream fields(text.substr(close + 2));
                std::string field;
                uint64_t ticks = 0;
                // utime and stime are fields 14 and 15; the substring starts at field 3.
                for (int i = 3; i <= 15 && fields >> field; ++i)
                    if (i >= 14) ticks += static_cast<uint64_t>(number(field, 0));
                current[tid] = ticks;
                const auto previous = threadTicks.find(tid);
                if (previous != threadTicks.end() && ticks >= previous->second && elapsed > .5 &&
                    elapsed < 30)
                    busiest = std::max(busiest, double(ticks - previous->second) /
                                                 sysconf(_SC_CLK_TCK) / elapsed);
            }
            if (!current.empty() && busiest > 0) s.threadPeak = std::clamp(busiest, 0., 1.);
            threadTicks = std::move(current);
            threadAt = mono;
            watchedAt = s.at;
        }
        // Mid-window reads must report the foreground just detected, not the previous one.
        if (!finishWindow) { s.app = app; s.awake = awake && !app.empty(); }
    }
    return s;
}
static long startSeconds(int pid) {
    auto text = readText("/proc/" + std::to_string(pid) + "/stat", 8192);
    auto end = text.rfind(')'); if (end == std::string::npos) return -1;
    std::istringstream in(text.substr(end + 2)); std::string field;
    for (int i = 3; i <= 22; ++i) if (!(in >> field)) return -1;
    // Shared lib.sh protocol stores integer seconds, rather than the raw tick count.
    return static_cast<long>(number(field, -100)) / sysconf(_SC_CLK_TCK);
}
ApplyLock::ApplyLock(const std::string& dir) : path(dir + "/.apply_lock") {
    if (mkdir(path.c_str(), 0700) != 0) {
        auto owner = readConfig(path + "/owner");
        struct stat st{};
        // A creator gets time to publish its PID. Never steal an ownerless fresh lock.
        if (stat(path.c_str(), &st) != 0 || time(nullptr) - st.st_mtime < 2) return;
        int pid = static_cast<int>(number(owner["pid"], -1));
        bool stale = owner["boot"] != trim(readText("/proc/sys/kernel/random/boot_id", 128)) || pid < 1 ||
            startSeconds(pid) != number(owner["start"], -2);
        if (!stale) return;
        unlink((path + "/owner").c_str());
        if (rmdir(path.c_str()) != 0 || mkdir(path.c_str(), 0700) != 0) return;
    }
    owned = atomicText(path + "/owner", "pid=" + std::to_string(getpid()) + "\nboot=" + trim(readText("/proc/sys/kernel/random/boot_id", 128)) + "\nstart=" + std::to_string(startSeconds(getpid())) + '\n');
    if (!owned) rmdir(path.c_str());
}
ApplyLock::~ApplyLock() {
    if (owned) { unlink((path + "/owner").c_str()); rmdir(path.c_str()); }
}
static std::vector<long> frequencyTable(const std::string& path) {
    std::istringstream in(readText(path, 4096)); std::vector<long> values; long value;
    while (in >> value) if (value > 0) values.push_back(value);
    std::sort(values.begin(), values.end()); values.erase(std::unique(values.begin(), values.end()), values.end());
    return values;
}
Actuator::Actuator(const std::string& dataDir, std::vector<Node> testNodes) : dir(dataDir), boot(trim(readText("/proc/sys/kernel/random/boot_id", 128))) {
    if (!testNodes.empty()) nodes = std::move(testNodes);
    else {
    for (const auto& p : globPaths("/sys/devices/system/cpu/cpufreq/policy*"))
        nodes.push_back({p + "/scaling_min_freq", p + "/scaling_max_freq", p + "/scaling_available_frequencies", 0});
    nodes.push_back({"/sys/kernel/gpu/gpu_min_clock", "/sys/kernel/gpu/gpu_max_clock", "/sys/kernel/gpu/gpu_freq_table", 1});
    for (const auto& p : globPaths("/sys/class/devfreq/*mif*"))
        nodes.push_back({p + "/min_freq", p + "/max_freq", p + "/available_frequencies", 2});
    nodes.push_back({"/proc/sys/kernel/sched_pelt_multiplier", "", "", 3});
    }
    // Separate "this kernel has no such node" from "something took the write bit away". The
    // second is recoverable with a chmod and is worth telling the user about; the first is not.
    // Both tests look at the mode, not at access(W_OK), which as root answers a question we are
    // not asking and would have let a locked node through to fail on the first write instead.
    for (const auto& n : nodes)
        if (access(n.path.c_str(), F_OK) == 0 && !ownerWritable(n.path))
            lockedPaths.push_back(n.path);
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
                               [](const Node& n) { return !ownerWritable(n.path); }), nodes.end());
    for (auto& n : nodes) {
        n.original = static_cast<long>(number(readText(n.path, 64), 0));
        n.maxOriginal = static_cast<long>(number(readText(n.maxPath, 64), 0));
        n.table = frequencyTable(n.tablePath);
    }
    // Recover only a journal from this kernel boot, and only for discovered allowlisted nodes.
    std::istringstream in(readText(dir + "/adaptive_journal", 65536));
    std::string version, savedBoot, path; long original, last;
    if (in >> version >> savedBoot && version == "M54_JOURNAL_1" && savedBoot == boot) {
        while (in >> path >> original >> last) {
            bool allowed = std::any_of(nodes.begin(), nodes.end(), [&](const Node& n) { return n.path == path || (n.axis == 1 && n.maxPath == path); });
            if (allowed && original > 0 && last > 0) journal[path] = {original, last};
        }
    }
}
bool Actuator::saveJournal() {
    std::ostringstream out; out << "M54_JOURNAL_1 " << boot << '\n';
    for (const auto& [path, entry] : journal) out << path << ' ' << entry.original << ' ' << entry.last << '\n';
    return atomicText(dir + "/adaptive_journal", out.str());
}
bool Actuator::writeOwned(const std::string& path, long value) {
    long current = static_cast<long>(number(readText(path, 64), -1));
    if (current == value) return true;
    if (current < 0 || value <= 0) return false;
    auto it = journal.find(path);
    if (it != journal.end() && current != it->second.last) return false; // owner interference
    if (it == journal.end()) journal[path] = {current, value};
    else it->second.last = value;
    // Write-ahead restoration journal: recoverable even after SIGKILL between sysfs writes.
    if (!saveJournal()) return false;
    if (!writeText(path, std::to_string(value) + '\n')) return false;
    return static_cast<long>(number(readText(path, 64), -1)) == value;
}
Constraints Actuator::constrain(Constraints c, const std::map<std::string, std::string>& cfg) const {
    auto has = [&](const std::string& key) { auto it = cfg.find(key); return it != cfg.end() && !it->second.empty(); };
    std::array<bool, 4> present{};
    for (const auto& n : nodes) {
        // PELT baseline is 2x; exploratory boost reaches 4x under heavy/rendering load.
        if (n.axis == 3 ? n.original < 4 : n.table.size() > 1) present[n.axis] = true;
    }
    for (int i = 0; i < 4; ++i) c.allowed[i] = c.allowed[i] && present[i];
    if (has("cpu_gov")) c.allowed[0] = false;
    if (has("gpu_min") || has("gpu_max") || has("gpu_gov") || has("gpu_power_policy")) c.allowed[1] = false;
    if (has("mif_min")) c.allowed[2] = false;
    auto pelt = cfg.find("adaptive_pelt");
    if (pelt != cfg.end() && pelt->second == "0") c.allowed[3] = false;
    return c;
}
bool Actuator::verified() const {
    for (const auto& [path, entry] : journal)
        if (static_cast<long>(number(readText(path, 64), -1)) != entry.last) return false;
    return true;
}
bool Actuator::apply(Action action, const Constraints& limits) {
    failureAxis = -1;
    // Re-read the baseline of every node we do NOT currently own. `original` was captured once,
    // at daemon start, from a live sysfs read -- and on this device scaling_min_freq is raised
    // transiently by Samsung's top-app QoS boost, so starting the daemon at the wrong moment
    // froze a boosted floor as "factory". Two things went wrong from that: every level below the
    // stale value became a no-op, which is how the big cluster stopped responding to the search
    // at all (journal captured original=1920000 where the module's own snapshot says 533000),
    // and restore() would have handed that boosted value back on the way out. A node we hold is
    // untouched here: its baseline lives in the journal, which is what restore actually reads.
    for (auto& n : nodes) {
        if (journal.count(n.path)) continue;
        long live = static_cast<long>(number(readText(n.path, 64), -1));
        if (live > 0) n.original = live;
        if (!n.maxPath.empty() && !journal.count(n.maxPath)) {
            long liveMax = static_cast<long>(number(readText(n.maxPath, 64), -1));
            if (liveMax > 0) n.maxOriginal = liveMax;
        }
    }
    for (const auto& n : nodes) {
        if (!limits.allowed[n.axis]) continue;
        failureAxis = n.axis;
        int level = action.level[n.axis];
        long desired = n.original;
        if (n.axis == 3) {
            long baseline = n.original >= 2 ? n.original : 2;
            desired = level == 0 ? baseline : (level >= 2 ? 4 : baseline);
        }
        else {
            if (n.table.empty()) continue;
            long cap = static_cast<long>(number(readText(n.maxPath, 64), 0));
            std::vector<long> table;
            for (auto x : n.table) if (x <= cap) table.push_back(x);
            if (table.empty()) return false;
            // Raise only floors, bounded by the CURRENT cooling/QoS cap. Never pin min==max.
            // The levels span the whole reachable table. At .15 per level the top of the search
            // asked for 60% of the table (.72 on MIF), which on this device sits BELOW the floors
            // the static profile has already written -- balanced leaves the big cluster at 2016000
            // and the GPU at 650000, while level 4 could only ask for 1536000 and 650000. Since a
            // floor is only ever raised, every level then resolved to the value already in place:
            // 4000 windows of search over an action space that could not move the hardware, and an
            // empty restoration journal to prove no write had ever been made. The min==max
            // invariant is held by the cap guard below, not by leaving the top of the range
            // unreachable.
            double fraction = level / double(Levels - 1);
            if (n.axis == 1 && limits.tier == Tier::Powersave) fraction = 0;
            auto index = static_cast<size_t>(fraction * (table.size() - 1));
            desired = level == 0 ? std::min(n.original, cap) : std::max(n.original, table[index]);
            if (desired >= cap && level > 0) desired = table.size() > 1 ? table[table.size() - 2] : table[0];
        }
        // Only axes handed to us are passed by the daemon; zero restores OUR owned writes.
        if (n.axis == 1 && limits.tier == Tier::Powersave && n.table.size() >= 3) {
            // Powersave explores GPU ceilings, preserving a usable CPU window and GPU floor.
            size_t idx = n.table.size() - 3 + std::min(2, level);
            long cap = std::min(n.maxOriginal, n.table[idx]);
            desired = std::min(n.original, cap);
            if (!writeOwned(n.path, desired) || !writeOwned(n.maxPath, cap)) return false;
            continue;
        }
        if (level == 0 && !journal.count(n.path)) continue;
        if (!writeOwned(n.path, desired)) return false;
    }
    if (!verified()) return false;
    failureAxis = -1;
    return true;
}
bool Actuator::restore() {
    bool ok = true;
    for (auto it = journal.begin(); it != journal.end();) {
        long current = static_cast<long>(number(readText(it->first, 64), -1));
        if (current == it->second.last) {
            if (!writeText(it->first, std::to_string(it->second.original) + '\n') ||
                number(readText(it->first, 64), -1) != it->second.original) { ok = false; ++it; continue; }
        }
        // If another owner/thermal QoS changed it, do not overwrite that owner's value.
        it = journal.erase(it);
    }
    for (auto& n : nodes) {
        if (!journal.count(n.path)) n.original = static_cast<long>(number(readText(n.path, 64), n.original));
        if (!journal.count(n.maxPath)) n.maxOriginal = static_cast<long>(number(readText(n.maxPath, 64), n.maxOriginal));
    }
    return saveJournal() && ok;
}
} // namespace m54
