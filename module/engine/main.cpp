#include "platform.hpp"
#include <algorithm>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
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
// Below this, a divergence between the two clocks is scheduling delay between the two reads,
// not a sleep any measurement has to care about. Real suspends here are seconds at the least.
static constexpr double SuspendGap = 1.;
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
        << "\ncpu_psi=" << s.cpuPsi << "\nmem_psi=" << s.memPsi << "\nio_psi=" << s.ioPsi
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
    if (stat(path.c_str(), &st) == 0 && st.st_size > limit) rotateGenerations(path, 3);
}
static constexpr char HistoryHeader[] =
    // `cadence` sits next to jank because jank is a share of intervals past 1500/cadence ms:
    // without the denominator in the file, the column cannot be compared between two sessions,
    // and a run whose cadence was detected higher reads as a run that got worse.
    // battery_temp belongs here as much as temp does: it vetoes every action at 43 C and is a
    // cost term, and it is also the sensor closest to what a hand on the glass feels, which is
    // the comparison that keeps getting made against a die sensor that is not measuring that.
    // `power_valid` next to `energy` for the same reason `cadence` sits next to `jank`: the
    // column means two different things depending on it -- measured watts over eight, or a
    // clock-and-load proxy that never saw a milliamp -- and a file that does not say which one
    // cannot be used to answer a question about battery.
    "at,profile,app,action,frames,cadence,cadence_seen,p95_ms,jank,temp,hot_zone,battery_temp,energy,power_valid,samples,windows,state_value,budget,"
    // The three pressures separately, not only folded into deficit_stall. The stall channel is
    // cpuPsi + 1.5*memPsi + ioPsi, and a window that stalled cannot be attributed from the sum:
    // "the texture pool was evicted" and "the storage could not keep up" are different problems
    // with different fixes, and they arrive at the objective as the same number.
    "queue_ms,queue_peak_ms,queue_late,reason,regime,deficit,deficit_stall,cpu_psi,mem_psi,io_psi,credit,"
    // Raw rates, unnormalised on purpose: the feature scales for these two are provisional, and
    // the point of logging them is to fit those scales to measured windows rather than guess again.
    "major_faults_s,swap_in_s,file_refault_s,"
    // Why more was not spent. `gate` names the term that closed exploration; want_* describe the
    // best costlier move the model could see. Together they separate a controller that learned an
    // axis is worthless from one that was never permitted to test it -- the two readings of "the
    // CPU axis never left level 1" that this file could not previously tell apart.
    "gate,want_move,want_adv,want_toll,want_tried,want_ok,axes,"
    // How much of this window's span the device spent suspended. Zero in almost every row, and
    // the one number that explains a budget that jumped without any window having earned it --
    // which is the reading that separates a cooled device from a controller that lost its clock.
    "suspended_s,"
    // How long this window actually ran. Not a constant since the window closes on frame
    // evidence rather than on the clock, and without it in the file a p95 measured over four and
    // a half seconds cannot be told from one measured over twelve -- which is precisely the
    // distinction the close rule exists to make. Same argument as `cadence` next to `jank`.
    "span_s";
// Rotate on size *or* on a schema change. Appending new columns to a file written by an older
// layout leaves the diagnostics export silently misaligned, which is worse than losing history.
// Rotations keep three generations instead of overwriting a single `.1`, so a long session
// keeps enough evidence to investigate rather than whatever survived last.
static std::ofstream openHistory(const std::string& path, off_t limit) {
    struct stat st{};
    bool keep = stat(path.c_str(), &st) == 0;
    if (keep && (st.st_size > limit ||
                 readText(path, 512).compare(0, std::strlen(HistoryHeader), HistoryHeader) != 0)) {
        rotateGenerations(path, 3);
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
// What a brain is valid for: this firmware on this kernel. Every OTA moves it.
static std::string engineIdentity() {
    utsname kernel{}; uname(&kernel);
    return std::to_string(hash(command({"getprop", "ro.build.fingerprint"}) + kernel.release + "m54-engine-3"));
}
int main(int argc, char** argv) {
    std::string mode = argc > 1 ? argv[1] : "--probe";
    std::string dir = argc > 2 ? argv[2] : "/data/adb/m54tuner";
    if (mode == "--probe") {
        Sampler sampler; sampler.read(120, false); pauseFor(1);
        auto s = sampler.read(120, true);
        std::cout << describe(s) << "conflict=" << processConflict() << '\n'; return 0;
    }
    if (getuid() != 0 || (mode != "--run" && mode != "--restore" && mode != "--adopt")) return 2;
    umask(0077); mkdir(dir.c_str(), 0700);
    int singleton = open((dir + "/adaptive.lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (singleton < 0 || flock(singleton, LOCK_EX | LOCK_NB) != 0) return 3;
    if (mode == "--adopt") {
        // Holding the singleton lock is what makes this safe: no daemon can save over the file
        // being replaced. Prints nothing on success, the reason on failure.
        const auto boot = readConfig(dir + "/config");
        const auto failed = adoptBrain(dir, engineIdentity(),
                                       legacyIdentities(boot, configIdentity(boot)),
                                       argc > 3 ? argv[3] : "");
        if (failed.empty()) return 0;
        std::cout << failed << '\n';
        return 6;
    }
    Actuator actuator(dir);
    {
        ApplyLock lock(dir);
        if (!lock || !actuator.restore()) return 4;
    }
    if (mode == "--restore") return 0;
    signal(SIGTERM, stop); signal(SIGINT, stop); signal(SIGHUP, stop);
    setpriority(PRIO_PROCESS, 0, 10);
    if (!atomicText(dir + "/adaptive_pid", pidRecord())) return 5;
    const std::string identity = engineIdentity();
    Brain brain;
    // A brain that does not load is set aside, never overwritten. The first periodic save used to
    // replace it with the empty brain this run starts from, so one loader bug (or a firmware
    // update moving `identity`) erased every window the device had learned, with nothing left to
    // recover from. Exported as `brain_rejected` so the reset is visible, not silent.
    std::string brainRejected;
    {
        const auto boot = readConfig(dir + "/config");
        brainRejected = loadBrain(dir, identity, legacyIdentities(boot, configIdentity(boot)), brain);
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
    // that keeps refusing is held off, and even then each count decays after fifteen quiet
    // minutes so a transient storm re-probes gradually instead of latching until restart.
    RefusalState refusalState;
    double windowAt = monotonic(), changedAt = 0, savedAt = 0, budgetAt = monotonic();
    // Suspend accounting. Both clocks advance together while the device is awake, so their
    // divergence across an iteration is exactly the time the device spent suspended. Accumulated
    // rather than read at the window boundary: a deep sleep can end on any iteration, including
    // the ones that close no window and `continue` straight back to the top.
    double wallAt = boottime(), tickAt = monotonic(), suspended = 0;
    double cpuSum = 0, gpuSum = 0, energySum = 0; int readings = 0;
    // Load is averaged across the window; temperature was whatever the last 1 Hz sample happened
    // to read at the boundary. Measured at rest on this device, the BIG and LITTLE die sensors
    // swing 5 C and step 3 C between half-second samples, so which sample landed on the boundary
    // decided the next window's thermal clamp. The window's PEAK, not its mean: this number
    // gates acting, and a peak is never less conservative than the single sample it replaces.
    double tempPeak = 0, batteryPeak = 0;
    Decision decision; std::string reason = "warming_up", credit = "none";
    // Per-window record of the exploration gate and of the best costlier move the model knows
    // about. `gate` names the term that closed exploration, because the terms are not
    // interchangeable: the battery one rises through a session and never falls back, so once it
    // shuts it stays shut, while the die one reopens whenever the scene lightens.
    std::string gate = "warming_up";
    Ambition want;
    double lastError = 0, lastValue = 0;
    Accuracy accuracy;   // how well the critic predicts real windows; see core.hpp
    double lastPairEnd = -1;   // `at` of the last learned pair's second window
    uint64_t rejected = 0, passiveWindows = 0;
    int rehearsed = 0;
    double lastRamTrimAt = -100;
    uint64_t totalRamTrims = 0;
    // Pending-trim state machine (unit-tested): one armed trim spans exactly the pair ending
    // at the next window, measured or not. "Unknown" and "measured zero" stay different facts
    // inside the gate.
    TrimGate trim;
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
        const double wall = boottime();
        // The two clocks are read a microsecond apart, so their difference is never exactly
        // zero; accumulating that would leave `suspended_s` full of 5e-06 and unreadable as the
        // "did this window sleep" column it exists to be. No suspend is shorter than this.
        const double gap = (wall - wallAt) - (tick - tickAt);
        suspended += gap > .05 ? gap : 0.;
        wallAt = wall; tickAt = tick;
        auto cfg = readConfig(dir + "/config");
        // One objective, across every workload. Legacy profile strings have no authority over
        // reward, acceptance, actuator semantics, session boundaries or whether learning runs.
        // Keep the history column for older diagnostic readers, explicitly labelled automatic.
        const std::string profile = "auto";
        auto control = value(cfg, "adaptive_mode", "active");
        if (!moduleRoot.empty() && (access((moduleRoot + "/disable").c_str(), F_OK) == 0 || access((moduleRoot + "/remove").c_str(), F_OK) == 0)) break;
        if (control == "off") { reason = "disabled"; break; }
        // Evidence, not the clock: see WindowFloor..WindowCeiling in platform.hpp for the
        // measured cliff this is built around. `pendingFrames()` is read before this poll's
        // read() runs, so it undercounts by at most one second of frames -- a window closed on
        // it holds at least WindowFrames, never fewer.
        bool endWindow = windowComplete(tick - windowAt, sampler.pendingFrames());
        // A user ceiling, not an assumption: the engine measures what the app actually renders
        // at and only refuses to chase anything above this.
        int cap = static_cast<int>(std::clamp(number(value(cfg, "adaptive_target_fps", "120"), 120), 24., 144.));
        auto s = sampler.read(cap, endWindow);
        cpuSum += s.cpu; gpuSum += s.gpu; energySum += s.energy; ++readings;
        tempPeak = std::max(tempPeak, s.temp); batteryPeak = std::max(batteryPeak, s.batteryTemp);
        if (!endWindow) { pauseFor(std::max(.05, 1. - (monotonic() - tick))); continue; }
        s.cpu = cpuSum / readings; s.gpu = gpuSum / readings; s.energy = energySum / readings;
        // What the sensor reads right now, before the window peak replaces it below. The peak is
        // the right input for charging the allowance -- effort is paid for at the worst heat it
        // produced -- and the wrong one for crediting a sleep, which refills against the
        // temperature on the way out. A phone put down hot carries its pre-suspend peak into this
        // window, so scoring the refill on it prices twenty cold minutes as if the die were still
        // at 75 C and hands back less than half the headroom the sleep actually earned.
        const double exitTemp = s.temp;
        if (s.thermalValid) { s.temp = std::max(s.temp, tempPeak); s.batteryTemp = std::max(s.batteryTemp, batteryPeak); }
        const double span = tick - windowAt;
        cpuSum = gpuSum = energySum = 0; readings = 0; tempPeak = batteryPeak = 0; windowAt = tick;
        Constraints baseLimits;
        baseLimits.tier = RuntimeObjective;
        baseLimits.high = std::clamp(number(value(cfg, "adaptive_thermal_limit", "82"), 82), 60., 84.);
        auto conflict = processConflict();
        // Re-checked every window rather than once at start: a lock can appear while running,
        // and a capability that disappears silently is exactly what went unnoticed for days.
        const auto locked = lockedTuningNodes();
        baseLimits = actuator.constrain(baseLimits, cfg);
        // The identity mask is the stable capability, never the transient backoff: feeding a
        // two-minute QoS hold into `context()` forked one device's physics across masks.
        decayRefusals(refusalState, tick);
        Constraints limits = withTransient(baseLimits, refusalState, tick);
        // Consumed once per closed window: the allowance below and the armed pair further down
        // are the two things a sleep invalidates, and both need the same number.
        const double slept = suspended; suspended = 0;
        // The allowance is charged for the effort that was actually in effect this window.
        allowance.update(current, s, limits, tick - budgetAt);
        // ...and credited for the part of the gap the device spent asleep. Without this the
        // allowance is frozen at whatever it held when the phone was put down: the hardware
        // cools to ambient across twenty minutes of deep sleep while `ceiling()` -- which clamps
        // every axis in safe() -- does not move, so the minutes after an unlock tune a cold
        // device as if it were still hot. It refills at the awake idle rate, .0025 per second,
        // so recovering a drained allowance took three minutes of USE where one minute of sleep
        // should already have paid for it. That gap was the bug, not the rate.
        if (slept > 0) { Observation cooled = s; cooled.temp = exitTemp; allowance.relax(cooled, limits, slept); }
        budgetAt = tick;
        brain.budget = allowance.remaining();
        limits.ceiling = allowance.ceiling();
        baseLimits.ceiling = limits.ceiling;
        auto configId = configIdentity(cfg);
        auto key = stableKey(s, baseLimits, configId);
        auto nextSession = s.app + ':' + profile + ':' + configId + ':' + conflict;
        bool transition = nextSession != session;
        bool bench = benchmark(dir);
        // Measure a previous trim at the next window, after asynchronous process teardown.
        // A spanned pair must not train the DVFS edge: its p95 and paging changes belong to
        // the release, not to the floor held across it.
        const bool trimSpanned = trim.closeWindow(s.memAvailKb);
        if (automaticRamTrimDue(cfg, s, transition, bench, tick - lastRamTrimAt)) {
            // Throttle unsuccessful attempts too; a failing binder command must not
            // be retried on every window of memory pressure.
            lastRamTrimAt = tick;
            if (trimBackgroundMemory()) {
                ++totalRamTrims;
                // ActivityManager acknowledges the request before process teardown and
                // page reclaim complete. Read its net effect at the NEXT window boundary.
                trim.arm(s.memAvailKb);
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
            if (slept > SuspendGap) {
                // The pair straddles a suspend: `before` and `s` are minutes or hours apart with
                // nothing measured in between, a warm device in one app backed up against a cold
                // one in another. The 30 s gate below now rejects the long sleeps on its own,
                // since `at` is boot time -- this catches the short suspends that fall inside its
                // tolerance and are still not transitions of anything.
                credit = "suspend_skipped"; eligible = false;
            } else if (trimSpanned) {
                // A trim landed inside this pair. Its p95 and paging changes belong to the
                // release, not to the floor held across it: value, policy and residual all
                // stay out, and the next window re-arms from the post-trim state.
                credit = "trim_skipped"; eligible = false;
            } else if (backupable(before, s)) {
                // Every sound window is a value backup, whether or not an axis moved, and
                // whether or not the user stayed in the same app. This is what makes learning
                // continuous instead of one sample per applied change in one quiet session.
                const auto measured = costs(s, limits, current);
                const double gain = reward(s, limits, current);
                const auto after = features(s, limits, current);
                const double step = std::pow(Discount, (s.at - before.at) / 6);
                const double predicted = brain.critic.value(beforeFeatures, beforeTier);
                lastError = brain.critic.learn(beforeFeatures, measured, after, beforeTier, 1, step);
                // The reward V is a discounted sum of: V = ValueLimit - 2 w.psi with psi the
                // discounted clipped costs, so r = 1 - 2 w.clip(c, 0, 4), not the clipped gain.
                double linearReward = 1;
                for (int i = 0; i < Costs; ++i)
                    linearReward -= 2 * preference(beforeTier)[i] * std::clamp(measured[i], 0., 4.);
                accuracy.observe(predicted, linearReward, step, brain.critic.value(after, beforeTier),
                                 before.at == lastPairEnd);
                lastPairEnd = s.at;
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
        // Cleared every window. These are only meaningful when the search actually ran, and a
        // value carried over from an earlier window would name a cause for a window that never
        // reached the gate -- `reason` already says why in that case.
        gate = "-";
        want = Ambition{};
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
            // Report the FIRST term that fails, in the order the condition evaluates them, so the
            // column names one cause rather than a set. Without this the history records only
            // that the controller stood still, and standing still because the model priced a
            // boost at zero and standing still because novelty was unavailable look identical.
            gate = !learning                        ? "learning_off"
                 : !(s.temp < limits.high - 10)     ? "die_hot"
                 : !(s.batteryTemp < 39)            ? "battery_hot"
                 : !(s.battery >= 25)               ? "charge_low"
                 : !(limits.ceiling >= 2)           ? "allowance"
                                                    : "open";
            decision = planner.search(brain, key, s, current, limits, explore);
            want = ambition(brain.model, key, s, current, limits, explore);
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
                        if (axis >= 0) axisRejected(refusalState, axis, tick);
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
               // Level 0 alone cannot distinguish no spending from a disabled axis.
               // Each refusal count decays after fifteen quiet minutes; reaching MaxRefusals
               // holds that axis off until a count decays. `axes_allowed` is the transient
               // permission the search used; `base_allowed` is the stable capability behind
               // the context identity. Export both so a backoff storm is distinguishable
               // from a device that never had the axis.
               << "\npelt_allowed=" << limits.allowed[3]
               << "\naxes_allowed=" << limits.allowed[0] << limits.allowed[1]
                                     << limits.allowed[2] << limits.allowed[3]
               << "\nbase_allowed=" << baseLimits.allowed[0] << baseLimits.allowed[1]
                                     << baseLimits.allowed[2] << baseLimits.allowed[3]
               << "\nrefusals=" << refusalState.count[0] << '/' << refusalState.count[1] << '/'
                                 << refusalState.count[2] << '/' << refusalState.count[3]
               << "\nrefusal_limit=" << MaxRefusals
               << "\naction=" << current.id() << "\nproposal=" << decision.action.id() << "\nmove=" << decision.move
               << "\nsamples=" << brain.model.samples << "\nwindows=" << brain.windows
               << "\nbrain_rejected=" << brainRejected
               << "\ncredit=" << credit << "\nrejected=" << rejected
               << "\nmodel_surprises=" << brain.model.surprises
               << "\ncontexts=" << brain.model.cells.size() << "\npolicies=" << brain.prior.contexts.size()
               << "\nreplay=" << brain.replay.samples.size() << "\nrehearsed=" << rehearsed
               << "\nsimulations=" << decision.simulations << "\ndepth=" << decision.depth
               << "\nplan_value=" << decision.value << "\nstate_value=" << lastValue
               << "\ntd_error=" << lastError << "\nconfidence=" << accuracy.value()
               << "\nconfidence_windows=" << accuracy.count()
               << "\nconfidence_state=" << (accuracy.state() == Accuracy::State::Warming ? "warming"
                                            : accuracy.state() == Accuracy::State::Flat ? "flat" : "scored")
               << "\nconfidence_variation=" << accuracy.variation()
               << "\ncritic_updates=" << brain.critic.updates
               << "\nbudget=" << allowance.remaining() << "\nceiling=" << limits.ceiling
               // The probe's own state belongs here: a silent failure leaving these at zero is
               // otherwise indistinguishable from a device with no scheduling delay at all.
               << "\nqueue_ms=" << s.queueMs << "\nqueue_peak_ms=" << s.queuePeakMs
               << "\nqueue_late=" << s.queueLate << "\nqueue_valid=" << s.queueValid
               << "\nprobe=" << sampler.probeReason() << "\nlocked_nodes=" << locked.size()
               << "\nowner=M54\ncapabilities=" << actuator.capabilities()
               << "\nmem_avail_mb=" << (s.memAvailKb > 0 ? s.memAvailKb / 1024 : -1)
               << "\nswap_total_mb=" << (s.swapTotalKb > 0 ? s.swapTotalKb / 1024 : -1)
               << "\nswap_free_mb=" << (s.swapFreeKb >= 0 && s.swapTotalKb > 0 ? s.swapFreeKb / 1024 : -1)
               << "\nram_trims=" << totalRamTrims
               << "\nlast_trim_freed_mb=" << (trim.measured ? std::to_string(trim.freedKb / 1024) : "unmeasured")
               << "\nsuspended_s=" << slept
               << "\nspan_s=" << span
               // Boot time, the same clock `adaptive_history.csv` stamps its `at` column with.
               // Every reader outside this process joins the two -- thermal_report against the
               // watch log, ab_report against its blocks, measure_ablation against /proc/uptime
               // for staleness -- and a monotonic value here silently offset all three by however
               // long the phone had slept since boot, which on a phone in real use is most of it.
               << "\nat=" << boottime() << '\n';
        atomicText(dir + "/adaptive_status", status.str());
        if ((brain.model.samples > 0 || brain.windows > 0) && tick - savedAt >= 60) {
            if (atomicText(dir + "/adaptive_model", brain.serialize(identity))) savedAt = tick;
        }
        auto history = openHistory(dir + "/adaptive_history.csv", 131072);
        history << s.at << ',' << profile << ',' << hash(s.app) << ',' << current.id() << ',' << s.frames << ','
                << s.target << ',' << s.cadenceSeen << ','
                << s.p95 << ',' << s.jank << ',' << s.temp << ','
                << (s.hotZone.empty() ? "-" : s.hotZone) << ',' << s.batteryTemp << ','
                << s.energy << ',' << s.powerValid << ',' << brain.model.samples << ','
                << brain.windows << ',' << lastValue << ',' << allowance.remaining() << ','
                << (s.queueValid ? s.queueMs : -1) << ',' << (s.queueValid ? s.queuePeakMs : -1) << ','
                << (s.queueValid ? s.queueLate : -1) << ',' << reason << ',' << regime << ',' << shortfall.primary()
                << ',' << (shortfall.valid[StallChannel] ? shortfall.value[StallChannel] : -1)
                << ',' << s.cpuPsi << ',' << s.memPsi << ',' << s.ioPsi
                << ',' << credit
                << ',' << (s.pagingValid ? s.majorFaults : -1)
                << ',' << (s.pagingValid ? s.swapIn : -1)
                << ',' << (s.pagingValid ? s.fileRefault : -1)
                << ',' << gate << ',' << want.move << ',' << want.advantage
                << ',' << want.toll << ',' << want.tried << ',' << (want.accepted ? 1 : 0)
                << ',' << (limits.allowed[0] * 1 + limits.allowed[1] * 2 +
                           limits.allowed[2] * 4 + limits.allowed[3] * 8)
                << ',' << slept << ',' << span << '\n';
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
