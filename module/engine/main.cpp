#include "platform.hpp"
#include <algorithm>
#include <csignal>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

using namespace m54;
static volatile sig_atomic_t stopping = 0;
static void stop(int) { stopping = 1; }
static void pauseFor(double seconds) {
    double until = monotonic() + seconds;
    while (!stopping && monotonic() < until) usleep(100000);
}
static std::string value(const std::map<std::string, std::string>& c, const std::string& key, const std::string& def) {
    auto it = c.find(key); return it == c.end() || it->second.empty() ? def : it->second;
}
static std::string describe(const Observation& s) {
    std::ostringstream out;
    // The measured cadence has to be visible: without it a late window and a genuinely slow
    // app produce the same p95, which is exactly the confusion that hid the burst misread.
    out << "app=" << s.app << "\nframes=" << s.frames << "\ncadence=" << s.target
        << "\ncadence_seen=" << s.cadenceSeen
        << "\np95_ms=" << s.p95 << "\njank=" << s.jank
        << "\nframe_start=" << std::setprecision(12) << s.frameStart
        << "\nframe_end=" << s.frameEnd << std::setprecision(6)
        << "\nframe_time_ms=" << s.frameTimeMs
        << "\nfps=" << (s.frameTimeMs > 0 ? 1000. * s.frames / s.frameTimeMs : 0)
        << "\nslow_frames_50ms=" << s.slowFrames50
        << "\ncpu=" << s.cpu << "\ncpu_peak=" << s.cpuPeak << "\nthread_peak=" << s.threadPeak
        << "\ngpu=" << s.gpu
        << "\ntemp=" << s.temp << "\nhot_zone=" << (s.hotZone.empty() ? "-" : s.hotZone)
        << "\nbattery_temp=" << s.batteryTemp
        << "\nwatts=" << (s.powerValid ? std::to_string(s.watts) : "unavailable")
        << "\nenergy_source=" << (s.powerValid ? "battery_mA_uV" : "clock_load_proxy")
        << "\nthermal_valid=" << s.thermalValid << "\nframes_valid=" << s.framesValid << "\nawake=" << s.awake
        << "\nmem_avail_kb=" << s.memAvailKb
        // Rates, not the raw counters: /proc/vmstat is monotonic since boot, so its absolute
        // values say nothing about this window. -1 marks a window that was not differenced.
        << "\nmajor_faults_s=" << (s.pagingValid ? s.majorFaults : -1)
        << "\nswap_in_s=" << (s.pagingValid ? s.swapIn : -1)
        << "\nfile_refault_s=" << (s.pagingValid ? s.fileRefault : -1)
        << "\npaging_valid=" << s.pagingValid << '\n';
    return out.str();
}
static std::string pidRecord() {
    auto text = readText("/proc/self/stat", 8192);
    auto end = text.rfind(')'); std::istringstream in(text.substr(end + 2)); std::string field;
    for (int i = 3; i <= 22; ++i) in >> field;
    auto boot = readText("/proc/sys/kernel/random/boot_id", 128);
    return "pid=" + std::to_string(getpid()) + "\nboot=" + boot + "start=" + std::to_string(static_cast<long>(number(field)) / sysconf(_SC_CLK_TCK)) + '\n';
}
static bool benchmark(const std::string& dir) {
    for (const auto& file : {"bench_pid", "bench_sched_pid"}) {
        auto r = readConfig(dir + '/' + file);
        int pid = static_cast<int>(number(r["pid"], 0));
        if (pid > 0 && kill(pid, 0) == 0 && readText("/proc/" + std::to_string(pid) + "/cmdline", 8192).find("bench") != std::string::npos) return true;
    }
    return false;
}
static void rotate(const std::string& path, off_t limit) {
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 && st.st_size > limit) rename(path.c_str(), (path + ".1").c_str());
}
static constexpr char HistoryHeader[] =
    // `cadence` sits next to jank because jank is a share of intervals past 1500/cadence ms:
    // without the denominator in the file, the column cannot be compared between two sessions,
    // and a run whose cadence was detected higher reads as a run that got worse.
    // battery_temp belongs here as much as temp does: it vetoes every action at 43 C and is a
    // cost term, and it is also the sensor closest to what a hand on the glass feels, which is
    // the comparison that keeps getting made against a die sensor that is not measuring that.
    "at,profile,app,action,frames,cadence,cadence_seen,p95_ms,jank,temp,hot_zone,battery_temp,energy,samples,windows,state_value,budget,"
    "queue_ms,queue_peak_ms,queue_late,reason,regime,deficit,deficit_stall,credit,"
    // Raw rates, unnormalised on purpose: the feature scales for these two are provisional, and
    // the point of logging them is to fit those scales to measured windows rather than guess again.
    "major_faults_s,swap_in_s,file_refault_s";
// Rotate on size *or* on a schema change. Appending new columns to a file written by an older
// layout leaves the diagnostics export silently misaligned, which is worse than losing history.
static std::ofstream openHistory(const std::string& path, off_t limit) {
    struct stat st{};
    bool keep = stat(path.c_str(), &st) == 0;
    if (keep && (st.st_size > limit ||
                 readText(path, 512).compare(0, std::strlen(HistoryHeader), HistoryHeader) != 0)) {
        rename(path.c_str(), (path + ".1").c_str());
        keep = false;
    }
    std::ofstream out(path, std::ios::app);
    if (!keep && out) out << HistoryHeader << '\n';
    return out;
}
// Training export: the only honest way to build an M54-specific model is to fit it on windows
// measured on this device. Rows are the exact features the critic sees, so an offline model can
// be compared against the online one without re-deriving the representation.
static void exportRow(const std::string& path, double at, Tier tier, uint64_t app,
                      const Features& before, int move, double gain, const Features& after) {
    rotate(path, 512 * 1024);
    std::ofstream out(path, std::ios::app);
    if (!out) return;
    out << std::setprecision(6) << at << ',' << static_cast<int>(tier) << ',' << app << ',' << move << ',' << gain;
    for (auto x : before) out << ',' << x;
    for (auto x : after) out << ',' << x;
    out << '\n';
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
static const char* const IdentityKeys[] = {
    "adaptive_target_fps", "adaptive_thermal_limit", "thermal", "fasrs_companion", "gos",
    "cpu_gov", "io_sched", "gpu_gov", "gpu_min", "gpu_max", "gpu_hs_load", "gpu_hs_clock",
    "gpu_hs_delay", "gpu_power_policy", "gpu_cl_boost", "gpu_dvfs_period", "gpu_polling_speed",
    "gpu_js_period", "mif_min", "int_min", "disp_min", "ufs_rpm_lvl", "f2fs_ipu", "fps_unlock",
    "samsung_perf", "samsung_spcm", "samsung_mars_off",
};
static std::string configIdentity(const std::map<std::string, std::string>& cfg) {
    std::ostringstream out;
    // `adaptive_pelt` is deliberately absent: which axes are permitted already reaches the
    // context as the allowed-axis mask, and hashing it here would say the same thing twice while
    // orphaning everything each time it is toggled.
    for (const char* key : IdentityKeys) {
        const auto found = cfg.find(key);
        out << key << '=' << (found == cfg.end() ? std::string() : found->second) << '\n';
    }
    return std::to_string(hash(out.str()));
}
// Identities this exact configuration would have produced under the previous rule. `profile` was
// inside that hash, so the same static surface yielded a different identity per profile; every
// one of those is provably the same surface as the current one and is rehomed onto it. An
// identity that matches none of them came from a configuration that cannot be reconstructed, and
// its cells are dropped instead of being folded in under a label they did not earn.
static std::map<std::string, std::string> legacyIdentities(std::map<std::string, std::string> cfg,
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
    return rehome;
}
int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "--probe";
    std::string dir = argc > 2 ? argv[2] : "/data/adb/m54tuner";
    if (mode == "--probe") {
        Sampler sampler; sampler.read(120, false); pauseFor(1);
        auto s = sampler.read(120, true); bool fas;
        std::cout << describe(s) << "conflict=" << processConflict(fas) << "\nfas=" << fas << '\n'; return 0;
    }
    if (getuid() != 0 || (mode != "--run" && mode != "--restore")) return 2;
    umask(0077); mkdir(dir.c_str(), 0700);
    int singleton = open((dir + "/adaptive.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (singleton < 0 || flock(singleton, LOCK_EX | LOCK_NB) != 0) return 3;
    Actuator actuator(dir);
    {
        ApplyLock lock(dir);
        if (!lock || !actuator.restore()) return 4;
    }
    if (mode == "--restore") return 0;
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGHUP, stop);
    setpriority(PRIO_PROCESS, 0, 10);
    if (!atomicText(dir + "/adaptive_pid", pidRecord())) return 5;
    utsname kernel{}; uname(&kernel);
    std::string identity = std::to_string(hash(command({"getprop", "ro.build.fingerprint"}) + kernel.release + "m54-engine-3"));
    Brain brain;
    {
        const auto boot = readConfig(dir + "/config");
        const auto current = configIdentity(boot);
        brain.deserialize(readText(dir + "/adaptive_model", 4 * 1024 * 1024), identity,
                          legacyIdentities(boot, current));
    }
    Budget allowance;
    allowance.restore(brain.budget);
    Planner planner(static_cast<uint32_t>(monotonic()));
    Observation before; Action previous, current;
    ContextKey beforeKey; Features beforeFeatures{}; std::vector<Choice> beforeOptions;
    int beforeMove = Stay;
    Tier beforeTier = RuntimeObjective;
    std::string session;
    bool eligible = false;
    bool cooling = false;
    // A refused write is usually Samsung's top-app QoS holding a cluster for a moment, not a
    // node we may never touch. Back off that axis for two minutes and try again; only a node
    // that keeps refusing is latched off for the rest of the run.
    std::array<double, Axes> blockedUntil{};
    std::array<int, Axes> refusals{};
    constexpr double Backoff = 120;
    constexpr int MaxRefusals = 5;
    double windowAt = monotonic(), changedAt = 0, savedAt = 0, budgetAt = monotonic();
    double cpuSum = 0, gpuSum = 0, energySum = 0; int readings = 0;
    // Load is averaged across the window; temperature was whatever the last 1 Hz sample happened
    // to read at the boundary. Measured at rest on this device, the BIG and LITTLE die sensors
    // swing 5 C and step 3 C between half-second samples, so which sample landed on the boundary
    // decided the next window's thermal clamp. The window's PEAK, not its mean: this number
    // gates acting, and a peak is never less conservative than the single sample it replaces.
    double tempPeak = 0, batteryPeak = 0;
    Decision decision; std::string reason = "warming_up", credit = "none";
    double lastError = 0, lastValue = 0;
    uint64_t rejected = 0, passiveWindows = 0;
    int rehearsed = 0;
    double lastRamTrimAt = -100;
    uint64_t totalRamTrims = 0;
    long lastRamFreedKb = 0;
    bool trimMeasured = false;     // "unknown" and "measured zero" are different facts
    long trimBaselineKb = 0;       // MemAvailable at the moment the pending trim was requested
    // A pair armed from a window the engine was not controlling. It still measured a real
    // transition of this device, so it earns a value backup; it earns nothing else, because the
    // engine did not choose to hold still there, it was not permitted to move.
    bool passive = false;
    // What was actually in effect when the pair was armed. If a thermal restore or an external
    // write moved it before the window closed, the value backup is still honest -- the state was
    // observed under whatever is in effect now -- but the edge credit no longer has an edge.
    Action armedAction;
    char executable[4096]{};
    auto pathLength = readlink("/proc/self/exe", executable, sizeof(executable) - 1);
    std::string moduleRoot = pathLength > 0 ? std::string(executable, pathLength) : "";
    moduleRoot = moduleRoot.substr(0, moduleRoot.rfind('/'));
    moduleRoot = moduleRoot.substr(0, moduleRoot.rfind('/'));
    Sampler sampler(moduleRoot);
    while (!stopping) {
        const double tick = monotonic();
        auto cfg = readConfig(dir + "/config");
        // One objective, across every workload. Legacy profile strings have no authority over
        // reward, acceptance, actuator semantics, session boundaries or whether learning runs.
        // Keep the history column for older diagnostic readers, explicitly labelled automatic.
        const std::string profile = "auto";
        auto control = value(cfg, "adaptive_mode", "active");
        if (!moduleRoot.empty() && (access((moduleRoot + "/disable").c_str(), F_OK) == 0 || access((moduleRoot + "/remove").c_str(), F_OK) == 0)) break;
        if (control == "off") { reason = "disabled"; break; }
        bool endWindow = tick - windowAt >= 6;
        // A user ceiling, not an assumption: the engine measures what the app actually renders
        // at and only refuses to chase anything above this.
        int cap = static_cast<int>(std::clamp(number(value(cfg, "adaptive_target_fps", "120"), 120), 24., 144.));
        auto s = sampler.read(cap, endWindow);
        cpuSum += s.cpu; gpuSum += s.gpu; energySum += s.energy; ++readings;
        tempPeak = std::max(tempPeak, s.temp); batteryPeak = std::max(batteryPeak, s.batteryTemp);
        if (!endWindow) { pauseFor(std::max(.05, 1. - (monotonic() - tick))); continue; }
        s.cpu = cpuSum / readings; s.gpu = gpuSum / readings; s.energy = energySum / readings;
        if (s.thermalValid) { s.temp = std::max(s.temp, tempPeak); s.batteryTemp = std::max(s.batteryTemp, batteryPeak); }
        cpuSum = gpuSum = energySum = 0; readings = 0; tempPeak = batteryPeak = 0; windowAt = tick;
        Constraints limits;
        limits.tier = RuntimeObjective;
        limits.high = std::clamp(number(value(cfg, "adaptive_thermal_limit", "82"), 82), 60., 84.);
        bool fas = false; auto conflict = processConflict(fas);
        // Re-checked every window rather than once at start: a lock can appear while running,
        // and a capability that disappears silently is exactly what went unnoticed for days.
        const auto locked = lockedTuningNodes();
        limits = actuator.constrain(limits, cfg, fas);
        for (int i = 0; i < Axes; ++i)
            limits.allowed[i] = limits.allowed[i] && refusals[i] < MaxRefusals && tick >= blockedUntil[i];
        // The allowance is charged for the effort that was actually in effect this window.
        allowance.update(current, s, limits, tick - budgetAt);
        budgetAt = tick;
        brain.budget = allowance.remaining();
        limits.ceiling = allowance.ceiling();
        auto configId = configIdentity(cfg);
        auto key = context(s, limits, configId);
        auto nextSession = s.app + ':' + profile + ':' + configId + ':' + std::to_string(fas) + ':' + conflict;
        bool transition = nextSession != session;
        bool bench = benchmark(dir);
        // Autonomous RAM Management:
        // Background cached apps hoard gigabytes of memory, starving UE5/graphics texture streaming pools,
        // triggering low-res mipmap drops, LOD cutbacks, and heavy ZRAM paging stalls.
        // Trim background cached apps proactively during memory starvation or upon demanding game launch.
        if (trimBaselineKb > 0 && s.memAvailKb > 0) {
            // Signed on purpose. Clamping at zero merges "the kill freed nothing" with "it freed
            // 200 MB and the foreground allocated 250 back inside the same window", and those
            // call for opposite conclusions about whether the trim is worth doing at all.
            lastRamFreedKb = s.memAvailKb - trimBaselineKb;
            trimBaselineKb = 0;
            trimMeasured = true;
        }
        bool enableRamTrim = value(cfg, "adaptive_ram_management", "1") == "1" ||
                             value(cfg, "game_ram_clear", "0") == "1";
        if (enableRamTrim && s.awake && !s.app.empty() && s.memAvailKb > 0) {
            bool renderWorkload = s.framesValid || s.frames > 0;
            bool demandingWorkload = demanding(s);
            bool severePressure = s.memPsi > 0.12 || s.memAvailKb < 800000;
            bool gamingShortage = (renderWorkload || demandingWorkload) && (s.memAvailKb < 1500000 || s.memPsi > 0.08);
            double cooldown = (s.memAvailKb < 600000) ? 20.0 : 60.0;
            if ((severePressure || gamingShortage) && (tick - lastRamTrimAt >= cooldown || (transition && (renderWorkload || demandingWorkload)))) {
                if (trimBackgroundMemory()) {
                    lastRamTrimAt = tick;
                    ++totalRamTrims;
                    // The effect is read at the NEXT window boundary, never inline. `kill-all`
                    // returns as soon as ActivityManager has acknowledged it, while process
                    // teardown and page reclaim run asynchronously afterwards -- so a
                    // MemAvailable read microseconds later is taken before a single page has
                    // come back. That is why every trim this engine has ever performed reported
                    // last_trim_freed_mb=0: an action with no measurement behind it.
                    trimBaselineKb = s.memAvailKb;
                }
            }
        }
        if (s.temp >= limits.high || s.batteryTemp >= limits.batteryHigh) cooling = true;
        if (cooling && s.thermalValid && s.temp <= limits.high - 5 && s.batteryTemp <= limits.batteryHigh - 2) cooling = false;
        bool safeNow = !cooling && s.awake && s.thermalValid && s.loadValid && s.temp < limits.high && s.batteryTemp < limits.batteryHigh && s.battery >= 10;
        bool verified = actuator.verified();
        bool learning = control == "active" && value(cfg, "adaptive_learning", "1") == "1";
        // Two separate permissions, deliberately not the same test. Acting needs the device to be
        // thermally safe and the window to be a rendering one. Learning needs only that the
        // measurement can be trusted. Tying them together is what made every frameless minute of
        // this phone's day invisible to the controller: no frames, no evidence, ever -- so it
        // could never find out whether it had anything to offer outside a game.
        bool canLearn = learning && verified && !bench && conflict.empty() && measurable(s);
        // Attribute only a stable, verified post-action window. Observe mode never trains on
        // actions it merely proposed; context changes and settling periods invalidate credit.
        if (eligible && canLearn) {
            if (backupable(before, s)) {
                // Every sound window is a value backup, whether or not an axis moved, and
                // whether or not the user stayed in the same app. This is what makes learning
                // continuous instead of one sample per applied change in one quiet session.
                const auto measured = costs(s, limits, current);
                const double gain = reward(s, limits, current);
                const auto after = features(s, limits, current);
                const double step = std::pow(Discount, (s.at - before.at) / 6);
                lastError = brain.critic.learn(beforeFeatures, measured, after, beforeTier, 1, step);
                lastValue = brain.critic.value(beforeFeatures, beforeTier);
                brain.replay.add(beforeFeatures, measured, after, beforeTier, step, planner.random());
                ++brain.windows;
                exportRow(dir + "/adaptive_dataset.csv", s.at, beforeTier, hash(before.app),
                          beforeFeatures, beforeMove, gain, after);
                if (passive) { ++passiveWindows; credit = "passive_value"; }
                else if (current != armedAction) credit = "action_changed";
                else if (!transition && attributable(before, s)) {
                    // A preference belongs to one app; a residual additionally blames one edge.
                    brain.prior.reinforce(beforeKey.policy, beforeTier, beforeMove, beforeOptions, lastError);
                    credit = brain.model.observe(beforeKey, before, previous, current, s) ? "learned" : "value_only";
                } else credit = "value_crossed";
                eligible = false;
            } else if (s.at - before.at > 30) { credit = "expired"; ++rejected; eligible = false; }
            // Otherwise keep the arming alive. One unusable window — the app changed and the
            // frame buffer was reset — used to throw the whole pair away, which is most of what
            // a phone in real use produces.
            else credit = "holding";
        } else if (eligible) { credit = "context_lost"; ++rejected; eligible = false; }
        bool canControl = safeNow && s.framesValid && conflict.empty() && !bench && control == "active";
        const Action measuredAction = current;
        // Idle, screen-off and frameless time is spent re-fitting the critic on measured windows
        // already in the buffer. Nothing imagined enters the buffer, and nothing is applied here.
        rehearsed = (!canControl && learning && brain.replay.samples.size() >= 32)
            ? brain.replay.rehearse(brain.critic, planner.random(), 32) : 0;
        if (transition || !canControl || !verified) {
            ApplyLock lock(dir);
            if (lock) {
                if (!actuator.restore()) {
                    atomicText(dir + "/adaptive_status", "mode=active\nreason=restore_pending\n");
                    pauseFor(1); continue;
                }
                current = {};
            }
            else { reason = "apply_busy"; pauseFor(1); continue; }
            // Only an app/layer change may reset presentation history (inside Sampler).
            // Resetting here replayed ~3 s of old SurfaceFlinger frames on EVERY observe
            // window, inflating its apparent FPS and changing its cadence/learning target.
            session = nextSession;
            if (transition || measuredAction != current) changedAt = tick;
            if (!verified) changedAt = tick;
        }
        if (!conflict.empty()) reason = "conflict_" + conflict;
        else if (bench) reason = "benchmark";
        // Ranked here rather than assigned above the chain, where it was overwritten by the very
        // next line and could therefore never be reported: in 922 recorded windows it appeared
        // zero times, which said nothing about how often another owner moved a node we held.
        else if (!verified) reason = "external_write";
        else if (!s.awake) reason = "screen_idle";
        else if (!safeNow) reason = "thermal_or_sensor_guard";
        else if (!s.framesValid) reason = "waiting_frames";
        else if (control == "observe") reason = "observe_only";
        else if (tick - changedAt < 18) reason = "settling";
        else reason = "mcts";
        if (rehearsed > 0 && (reason == "screen_idle" || reason == "waiting_frames")) reason = "rehearsing";
        if (safeNow && !bench && conflict.empty() && s.framesValid && !transition && tick - changedAt >= 18) {
            // Exploration is gated on safety and headroom, never on whether the session is
            // currently smooth: a bad plateau is exactly where the controller has to be allowed
            // to look around. Only then does the search add root noise over its learned prior.
            //
            // It also requires learning to be on. Trying an axis and then not recording what
            // happened is pure cost, and it is what makes a still-learning controller measure
            // worse than doing nothing: the comparison then prices the tuition, not the result.
            // Freezing the memory therefore also freezes exploration, which is what makes an
            // exploit-only measurement possible at all.
            const bool explore = learning && s.temp < limits.high - 10 && s.batteryTemp < 39 &&
                s.battery >= 25 && limits.ceiling >= 2;
            decision = planner.search(brain, key, s, current, limits, explore);
            auto next = accept(brain.model, key, s, current, decision.action, limits, explore)
                ? decision.action : current;
            if (canControl) {
                ApplyLock lock(dir);
                if (lock && configId == configIdentity(readConfig(dir + "/config"))) {
                    previous = current;
                    if (actuator.apply(next, limits)) {
                        beforeOptions = candidates(previous, s, limits);
                        beforeMove = moveIndex(previous, next, s, limits);
                        current = next;
                        if (current != previous) changedAt = tick;
                        before = s; beforeKey = key; beforeTier = limits.tier;
                        beforeFeatures = features(s, limits, previous);
                        armedAction = current; passive = false;
                        eligible = beforeMove >= 0;
                    } else {
                        const int axis = actuator.rejectedAxis();
                        if (axis >= 0) { blockedUntil[axis] = tick + Backoff; ++refusals[axis]; }
                        bool restored = actuator.restore();
                        current = {}; changedAt = tick;
                        reason = restored ? "write_rejected" : "restore_pending";
                    }
                } else reason = "apply_busy";
            }
        } else if (canControl && !transition && s.framesValid && tick - changedAt >= 6) {
            // At least one complete window after each change; overlapping frame buffers are
            // de-duplicated by present timestamp in FrameTracker. Holding still is also a
            // decision, and it earns a value backup like any other.
            before = s; beforeKey = key; beforeTier = limits.tier; previous = current;
            beforeOptions = candidates(current, s, limits);
            beforeMove = Stay;
            beforeFeatures = features(s, limits, current);
            armedAction = current; passive = false;
            eligible = true;
        } else if (canLearn && !eligible && !transition && tick - changedAt >= 6) {
            // Not controlling this window. The actuator is released, so the action is known and
            // empty, and the window is a real measured transition of this device under it -- a
            // download, a compile, a sync, a screen-off workload, a game the engine was barred
            // from touching. That is a value backup like any other. It is NOT a policy signal:
            // holding still here was imposed, not chosen, and reinforcing a forced move as a
            // preferred one teaches a controller to like its own restrictions.
            before = s; beforeKey = key; beforeTier = limits.tier; previous = current;
            beforeOptions = candidates(current, s, limits);
            beforeMove = Stay;
            beforeFeatures = features(s, limits, current);
            armedAction = current; passive = true;
            eligible = true;
        }
        const auto shortfall = deficit(s);
        // Which estimator the objective actually read this window, so no report has to guess
        // whether a zero deficit meant "served" or "never measured".
        const char* regime = shortfall.valid[FrameChannel] ? "render"
            : shortfall.valid[StallChannel] ? "compute" : "unmeasured";
        std::ostringstream status;
        status << "mode=" << control << "\nreason=" << reason << "\nprofile=" << profile
               << "\nobjective=service_cost_v1\nlearning_enabled=" << learning << '\n' << describe(s)
               << "regime=" << regime
               << "\ndeficit=" << shortfall.primary()
               << "\ndeficit_frame=" << (shortfall.valid[FrameChannel] ? shortfall.value[FrameChannel] : -1)
               << "\ndeficit_queue=" << (shortfall.valid[QueueChannel] ? shortfall.value[QueueChannel] : -1)
               << "\ndeficit_stall=" << (shortfall.valid[StallChannel] ? shortfall.value[StallChannel] : -1)
               << "\ndemanding=" << demanding(s) << "\ncan_learn=" << canLearn
               // The two halves of canLearn, separately. 210 of 922 windows armed an action and
               // then dropped the measurement as "context_lost", and with only the verdict
               // exported there was no way to tell an unmeasurable window from a node another
               // owner had moved under us.
               << "\nnodes_verified=" << verified << "\nmeasurable=" << measurable(s)
               << "\ncan_control=" << canControl << "\npassive_windows=" << passiveWindows
               << "\nmeasured_action=" << measuredAction.id()
               << "\npelt_allowed=" << limits.allowed[3]
               << "\naction=" << current.id() << "\nproposal=" << decision.action.id() << "\nmove=" << decision.move
               << "\nsamples=" << brain.model.samples << "\nwindows=" << brain.windows
               << "\ncredit=" << credit << "\nrejected=" << rejected
               << "\nmodel_surprises=" << brain.model.surprises
               << "\ncontexts=" << brain.model.cells.size() << "\npolicies=" << brain.prior.contexts.size()
               << "\nreplay=" << brain.replay.samples.size() << "\nrehearsed=" << rehearsed
               << "\nsimulations=" << decision.simulations << "\ndepth=" << decision.depth
               << "\nplan_value=" << decision.value << "\nstate_value=" << lastValue
               << "\ntd_error=" << lastError << "\nconfidence=" << brain.critic.trust()
               << "\nbudget=" << allowance.remaining() << "\nceiling=" << limits.ceiling
               // The probe's own state belongs here: a silent failure leaving these at zero is
               // otherwise indistinguishable from a device with no scheduling delay at all.
               << "\nqueue_ms=" << s.queueMs << "\nqueue_peak_ms=" << s.queuePeakMs
               << "\nqueue_late=" << s.queueLate << "\nqueue_valid=" << s.queueValid
               << "\nprobe=" << sampler.probeReason() << "\nlocked_nodes=" << locked.size()
               << "\nowner=" << (fas ? "fas-rs+M54" : "M54") << "\ncapabilities=" << actuator.capabilities()
               << "\nmem_avail_mb=" << (s.memAvailKb > 0 ? s.memAvailKb / 1024 : -1)
               << "\nram_trims=" << totalRamTrims
               << "\nlast_trim_freed_mb=" << (trimMeasured ? std::to_string(lastRamFreedKb / 1024) : "unmeasured")
               << "\nat=" << monotonic() << '\n';
        atomicText(dir + "/adaptive_status", status.str());
        if ((brain.model.samples > 0 || brain.windows > 0) && tick - savedAt >= 60) {
            if (atomicText(dir + "/adaptive_model", brain.serialize(identity))) savedAt = tick;
        }
        auto history = openHistory(dir + "/adaptive_history.csv", 131072);
        history << s.at << ',' << profile << ',' << hash(s.app) << ',' << current.id() << ',' << s.frames << ','
                << s.target << ',' << s.cadenceSeen << ','
                << s.p95 << ',' << s.jank << ',' << s.temp << ','
                << (s.hotZone.empty() ? "-" : s.hotZone) << ',' << s.batteryTemp << ','
                << s.energy << ',' << brain.model.samples << ','
                << brain.windows << ',' << lastValue << ',' << allowance.remaining() << ','
                << (s.queueValid ? s.queueMs : -1) << ',' << (s.queueValid ? s.queuePeakMs : -1) << ','
                << (s.queueValid ? s.queueLate : -1) << ',' << reason << ',' << regime << ',' << shortfall.primary()
                << ',' << (shortfall.valid[StallChannel] ? shortfall.value[StallChannel] : -1)
                << ',' << credit
                << ',' << (s.pagingValid ? s.majorFaults : -1)
                << ',' << (s.pagingValid ? s.swapIn : -1)
                << ',' << (s.pagingValid ? s.fileRefault : -1) << '\n';
        pauseFor(std::max(.05, 1. - (monotonic() - tick)));
    }
    if (brain.model.samples > 0 || brain.windows > 0) atomicText(dir + "/adaptive_model", brain.serialize(identity));
    bool restored = false;
    for (int i = 0; i < 20 && !restored; ++i) {
        { ApplyLock lock(dir); if (lock) restored = actuator.restore(); }
        if (!restored) usleep(100000);
    }
    atomicText(dir + "/adaptive_status", "mode=off\nreason=" + std::string(restored ? "stopped_restored" : "restore_pending") +
        "\nsamples=" + std::to_string(brain.model.samples) + "\nwindows=" + std::to_string(brain.windows) + '\n');
    unlink((dir + "/adaptive_pid").c_str());
    close(singleton);
    return restored ? 0 : 6;
}
