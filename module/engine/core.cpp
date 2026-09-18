#include "core.hpp"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <iomanip>
#include <limits>
#include <sstream>

namespace m54 {
static double clip(double x, double lo, double hi) { return std::clamp(x, lo, hi); }
// Parses one persisted per-feature gradient scale. Strict about malformed text, NaN/inf
// and overflow, but a denormal underflow reads as the zero it behaves as: the scale is
// the only multiplicatively-decayed float the brain persists (variance *= beta), so it
// is the only one that can decay past the smallest normal double — every other persisted
// float moves additively and stays in range, and stays fail-fast here. Canonicalising to
// exactly 0.0 (rather than keeping the subnormal) keeps the text identical on every libc.
bool parseScaleToken(const std::string& token, double& out) {
    if (token.empty()) return false;
    errno = 0;
    char* end = nullptr;
    const double v = std::strtod(token.c_str(), &end);
    if (end == token.c_str() || *end != '\0' || !std::isfinite(v)) return false;
    if (errno == ERANGE && std::fabs(v) > 1.0) return false; // overflow stays fatal
    out = (v != 0.0 && std::fabs(v) < std::numeric_limits<double>::min()) ? 0.0 : v;
    return true;
}
static constexpr int MaxEffort = Axes * (Levels - 1);

int Action::id() const { return level[0] + 5 * level[1] + 25 * level[2] + 125 * level[3]; }
Action Action::fromId(int id) {
    Action a;
    for (auto& x : a.level) { x = id % Levels; id /= Levels; }
    return a;
}
int Action::effort() const { return level[0] + level[1] + level[2] + level[3]; }

uint64_t hash(const std::string& text) {
    uint64_t h = 14695981039346656037ULL;
    for (unsigned char c : text) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
ContextKey context(const Observation& s, const Constraints& c, const std::string& configId) {
    int mask = 0;
    for (int i = 0; i < Axes; ++i) if (c.allowed[i]) mask |= 1 << i;
    std::ostringstream coarse;
    // Coarse identity: WHAT IS BEING TUNED, not what the user wants out of it. The profile is
    // deliberately absent. Raising a GPU floor one step changes this app's p95 by whatever it
    // changes it by; that number does not depend on whether the user picked Economia this
    // afternoon. Keying the transition model by profile meant learning the same silicon twice,
    // and it made switching profiles look like arriving on an unknown device.
    coarse << hash(s.app) << ':' << s.target << ':' << s.charging << ':' << s.powerValid << ':'
           << mask << ':' << configId;
    std::ostringstream fine;
    fine << coarse.str() << '|' << static_cast<int>(s.temp / 8) << ':'
         << static_cast<int>(s.cpu * 3) << ':' << static_cast<int>(s.gpu * 3) << ':'
         << (s.memPsi > .05);
    // The policy key does carry it: which move is preferred is exactly the question the
    // objective answers differently.
    return {fine.str(), coarse.str(), std::to_string(static_cast<int>(c.tier)) + ':' + coarse.str()};
}
static bool finite(const Observation& s) {
    for (double v : {s.at, s.cpu, s.gpu, s.cpuPsi, s.memPsi, s.ioPsi, s.temp,
                     s.batteryTemp, s.trend, s.watts, s.energy, s.p95, s.jank})
        if (!std::isfinite(v)) return false;
    return true;
}
bool Deficit::any() const {
    for (int i = 0; i < Channels; ++i) if (valid[i]) return true;
    return false;
}
double Deficit::primary() const {
    if (valid[FrameChannel]) return value[FrameChannel];
    if (valid[StallChannel]) return value[StallChannel];
    return 0;
}
Deficit deficit(const Observation& s) {
    Deficit d;
    // Frame lateness: p95 presentation interval against the cadence the app actually delivers.
    // Valid only while a renderer is presenting; this is the estimator the controller has
    // evidence for, and the only one it is allowed to act on today.
    d.valid[FrameChannel] = s.framesValid && s.p95 > 0 && s.target > 0;
    if (d.valid[FrameChannel]) d.value[FrameChannel] = clip(s.p95 * s.target / 1000. - 1, 0, 3) / 3;
    // Runqueue lateness of the foreground app's threads, from the eBPF probe. Carried into the
    // feature vector so the critic may find it; kept out of the objective until it demonstrates
    // correlation with observed stutter on this device. Measured here: 1.4% late in NTE where
    // threads wait for CPU, 0.08% in Roblox where they never stop running.
    d.valid[QueueChannel] = s.queueValid;
    if (d.valid[QueueChannel])
        d.value[QueueChannel] = clip(std::max(s.queueMs / 8, s.queueLate * 8), 0, 1);
    // Stall: the share of the window in which work existed and could not proceed for want of a
    // resource. The one deficit estimator that needs no renderer, no frame and no lit screen,
    // which is exactly why it is what a frameless workload has to be judged by.
    d.valid[StallChannel] = s.loadValid;
    if (d.valid[StallChannel])
        d.value[StallChannel] = clip(s.cpuPsi + 1.5 * s.memPsi + s.ioPsi, 0, 1);
    return d;
}
// Thresholds are judgement, not measurement. They only have to sit above the noise floor of a
// phone with nothing to do; anything above them is a window where some answer was better than
// another, and therefore a window worth a value backup.
bool demanding(const Observation& s) {
    return s.framesValid || s.cpu > .08 || s.cpuPeak > .25 || s.threadPeak > .25 || s.gpu > .05 ||
        s.cpuPsi > .02 || s.memPsi > .01 || s.ioPsi > .02 || (s.queueValid && s.queueMs > .5);
}
bool measurable(const Observation& s) {
    return finite(s) && s.thermalValid && s.loadValid && demanding(s) && deficit(s).any();
}
// Both windows individually sound, under the same objective. Deliberately says nothing about
// how much the workload moved: load, pressure and temperature are already in the feature
// vector, so a window where the phone went from idle to a cold start is a real transition of
// this device and not a confound. Gating on a calm workload is how a controller ends up trained
// only on the easy half of the day and surprised by the other half.
static bool sound(const Observation& a, const Observation& b) {
    if (!measurable(a) || !measurable(b)) return false;
    // The charge path changes what energy costs and what the thermal ceiling means, so a pair
    // that straddles it is comparing two different devices.
    if (a.charging != b.charging || a.powerValid != b.powerValid) return false;
    if (b.at - a.at < 4 || b.at - a.at > 30) return false;
    // The cadence target is a property of a rendering workload. Requiring it to match across a
    // pair where one side rendered and the other did not asks a question that has no answer;
    // it is required only where both sides actually have a frame channel.
    if (a.framesValid && b.framesValid && a.target != b.target) return false;
    if (b.framesValid && !(b.jank >= 0 && b.jank <= 1)) return false;
    return true;
}
// Valid for a value backup. The app may differ across the pair: the critic reads the workload
// — lateness, jank, load, pressure, heat — and never the package name, so leaving one app for
// another is an ordinary transition of this device. Refusing those pairs means a phone in real
// use, where the foreground changes every few seconds, teaches the critic nothing at all.
bool backupable(const Observation& a, const Observation& b) { return sound(a, b); }
// Valid for crediting a policy preference, which is keyed per app and cannot cross that line.
bool attributable(const Observation& a, const Observation& b) {
    return sound(a, b) && !a.app.empty() && a.app == b.app;
}
// Valid for a per-edge residual, which does blame one applied change for what followed. Here a
// workload that moved underneath the measurement genuinely destroys the attribution.
bool learnable(const Observation& a, const Observation& b) {
    return attributable(a, b) && a.framesValid && b.framesValid && a.frames >= 24 && b.frames >= 24 &&
        std::abs(b.cpu - a.cpu) <= .35 && std::abs(b.gpu - a.gpu) <= .40 &&
        std::abs(b.temp - a.temp) < 8;
}
const CostVector& preference(Tier tier) {
    // Three objectives, not three labels. Only these numbers differ between profiles; every
    // quantity they multiply is measured the same way regardless of which one is selected.
    // The thermal and battery terms are identical in all three on purpose: a limit is a limit.
    static const std::array<CostVector, 3> weights{{
        //  late  jank  energy pressure heat rising battery breach effort
        {{ .48,   .28,  .08,   .08,     .22, .12,   .30,    1.0,   .005 }}, // Game
        {{ .32,   .23,  .25,   .10,     .22, .12,   .30,    1.0,   .025 }}, // Balanceado
        {{ .22,   .20,  .43,   .08,     .22, .12,   .30,    1.0,   .040 }}, // Economia
    }};
    return weights[static_cast<size_t>(tier)];
}
CostVector costs(const Observation& s, const Constraints& c, Action applied) {
    CostVector k{};
    if (!finite(s)) { k[BreachCost] = 1; k[LateCost] = 1; return k; }
    k[LateCost] = deficit(s).primary();
    k[JankCost] = deficit(s).valid[FrameChannel] ? clip(s.jank, 0, 1) : 0;
    k[EnergyCost] = clip(s.energy, 0, 1.5);
    k[PressureCost] = clip(s.cpuPsi + 1.5 * s.memPsi + s.ioPsi, 0, 1);
    k[HeatCost] = clip((s.temp - (c.high - 12)) / 12, 0, 2);
    k[RisingCost] = clip(s.trend / .35, 0, 1);
    k[BatteryCost] = clip((s.batteryTemp - (c.batteryHigh - 4)) / 4, 0, 2);
    k[BreachCost] = (!s.thermalValid || s.temp >= c.high || s.batteryTemp >= c.batteryHigh) ? 1 : 0;
    // Minimal intervention: among settings that meet the objective, prefer the cheapest. Without
    // it, an unmeasurable energy proxy lets the search park every axis at maximum for no measured
    // gain. Game barely pays it; Economia pays it heavily, which is the whole point of asking for
    // Economia. The thermal and battery limits are not part of this trade.
    k[EffortCost] = applied.effort() / double(MaxEffort);
    return k;
}
static double assemble(const CostVector& k, Tier tier) {
    const auto& w = preference(tier);
    double total = 0;
    for (int i = 0; i < Costs; ++i) total += w[i] * k[i];
    return clip(1 - 2 * total, -1, 1);
}
double reward(const Observation& s, const Constraints& c) { return reward(s, c, Action{}); }
double reward(const Observation& s, const Constraints& c, Action applied) {
    if (!finite(s)) return -1;
    return assemble(costs(s, c, applied), c.tier);
}
Features features(const Observation& s, const Constraints& c, Action applied) {
    Features f{};
    const auto d = deficit(s);
    f[0] = 1;
    f[1] = d.valid[FrameChannel] ? d.value[FrameChannel] : 0;
    f[2] = d.valid[FrameChannel] ? clip(s.jank, 0, 1) : 0;
    f[3] = clip(s.energy / 1.5, 0, 1);
    f[4] = clip((s.temp - (c.high - 12)) / 12, 0, 2) / 2;
    f[5] = clip(s.trend / .35, 0, 1);
    f[6] = clip((s.batteryTemp - (c.batteryHigh - 4)) / 4, 0, 2) / 2;
    f[7] = clip(s.cpu, 0, 1);
    f[8] = clip(s.gpu, 0, 1);
    f[9] = clip(s.cpuPsi, 0, 1);
    f[10] = clip(s.memPsi * 2, 0, 1);
    f[11] = clip(s.ioPsi, 0, 1);
    f[12] = clip(s.battery / 100., 0, 1);
    f[13] = s.charging ? 1 : 0;
    for (int i = 0; i < Axes; ++i) f[14 + i] = applied.level[i] / double(Levels - 1);
    // Whether the bottleneck is one saturated thread or spread across the device. The critic
    // cannot tell those apart from aggregate load, and they call for opposite answers: a
    // saturated thread wants ramp speed and placement, a spread load wants headroom. Measured
    // on Roblox as cpu=0.31, cpu_peak=0.52, thread_peak=1.00 in the same window.
    f[18] = clip(s.cpuPeak, 0, 1);
    f[19] = clip(s.threadPeak, 0, 1);
    // Which scenario this is. Without f[20] the critic cannot tell a window with no frame
    // measurement from a window whose frames were paced perfectly -- both arrive as f[1] = 0 --
    // so every frameless minute of the day taught it that the workload was ideal. A controller
    // asked to situate itself in whatever scenario it is in has to be told which one that is,
    // and it needs the validity flag next to the value, not folded into it.
    //
    // They are ABSENCE flags, zero in the rendering, screen-on case, and that is not cosmetic.
    // As presence flags they read 1 in every window of a render-only session, which makes them
    // exact copies of the bias f[0]. The per-feature step normalisation divides each gradient by
    // its own scale, so a constant column moves its weight by a full step every window whatever
    // its magnitude -- and three copies of the bias move the constant part of the value three
    // times per window instead of once. Measured in tests/engine_sim.cpp: as presence flags the
    // hot-device case lost a third of its learnable windows to the resulting instability and
    // recovered completely when they read zero in the regime they do not describe.
    //
    // That same argument is why the queue channel gets no absence flag of its own. It is
    // constant on any one device -- the probe either attaches for the whole run or never does --
    // so it would be a third copy of the bias while telling the critic nothing it could use. The
    // cost is that "no probe" and "no scheduling delay" reach f[21] as the same number, which is
    // acceptable only because the queue channel is excluded from the objective. The day it earns
    // a place there, the flag has to come back, and the update rule has to stop treating a
    // constant column as a full-strength gradient direction.
    f[20] = d.valid[FrameChannel] ? 0 : 1;
    f[21] = d.valid[QueueChannel] ? d.value[QueueChannel] : 0;
    f[22] = d.valid[StallChannel] ? d.value[StallChannel] : 0;
    f[23] = s.awake ? 0 : 1;
    // Stutter counted against a fixed 50 ms threshold rather than against the detected cadence:
    // independent of whether the cadence estimate itself is right.
    f[24] = d.valid[FrameChannel] ? clip(s.slowFrames50 / 20., 0, 1) : 0;
    // Paging, on exactly the terms f[21] is on: in the features, out of the objective, until it
    // has demonstrated correlation with observed stutter on this device's own windows. It gets no
    // absence flag for the reason given above -- /proc/vmstat is readable in every window of every
    // run here, so the flag would be a third copy of the bias while telling the critic nothing.
    //
    // The scales are the measured p99 of each channel over 483 windows of real use on this
    // device (tools/paging_report.py, 2026-09-09): swap-in 7122/s, file refault 44617/s. The
    // first guesses -- 2000 and 20000 -- clipped the top decile of exactly the signal being
    // evaluated, which is the one part of the range where the question is decided. Re-fit from
    // the report rather than from intuition if the workload mix changes.
    f[25] = s.pagingValid ? clip(s.swapIn / 8000., 0, 1) : 0;
    f[26] = s.pagingValid ? clip(s.fileRefault / 45000., 0, 1) : 0;
    return f;
}
Action safe(Action a, const Observation& s, const Constraints& c) {
    if (!s.awake || !s.thermalValid || !s.loadValid || s.temp >= c.high ||
        s.batteryTemp >= c.batteryHigh || s.battery < 10) return {};
    // No per-profile ceiling. A profile is a price list, not a fence: EffortCost already makes
    // level 4 cost Economia eight times what it costs Game, and the acceptance gate still demands
    // a modelled margin before spending. Capping the search by decree forecloses exactly the
    // evidence that would settle whether the expensive setting was worth it, which is the one
    // thing a search over a learned model exists to find out. What remains are real limits:
    // thermal proximity, charge, and the earned allowance.
    int max = Levels - 1;
    if (s.temp + std::max(0., s.trend) * 12 >= c.high - 3 || s.battery < 20) max = 1;
    max = std::min(max, std::clamp(c.ceiling, 0, Levels - 1));
    // PELT has two settings, not five: the 2x baseline and the 4x boost, which the actuator only
    // writes from level 2 up. Clamping this axis at 1 made that boost unreachable -- the branch
    // that writes 4 could never be selected by the planner, only by a direct actuator test -- so
    // the axis cost effort and earned credit while never changing the multiplier. Clamped at 2
    // instead, the thermal rule above still holds it at baseline exactly when it matters: max
    // drops to 1 near the limit, and level 1 is the baseline.
    for (int i = 0; i < Axes; ++i)
        a.level[i] = c.allowed[i] ? std::clamp(a.level[i], 0, i == 3 ? std::min(2, max) : max) : 0;
    // Stale/missing frames do not justify exploratory boosts.
    if (!s.framesValid) for (auto& x : a.level) x = 0;
    return a;
}
std::vector<Choice> candidates(Action current, const Observation& s, const Constraints& c) {
    current = safe(current, s, c);
    std::vector<Choice> out;
    out.push_back({Stay, current});
    auto known = [&](const Action& a) {
        return std::any_of(out.begin(), out.end(), [&](const Choice& x) { return x.action == a; });
    };
    for (int move = 1; move < Release; ++move) {
        auto a = current;
        a.level[(move - 1) / 2] += (move - 1) % 2 ? 1 : -1;
        a = safe(a, s, c);
        if (!known(a)) out.push_back({move, a});
    }
    if (!known(Action{})) out.push_back({Release, {}});
    return out;
}
int moveIndex(Action from, Action to, const Observation& s, const Constraints& c) {
    for (const auto& choice : candidates(from, s, c)) if (choice.action == to) return choice.move;
    return -1;
}

// How hot the device is relative to the point where effort starts costing anything, and the
// rate the allowance moves at, per second, for a given effort at that heat. Both live here
// rather than inline in each caller because update() and relax() have to agree exactly: a
// suspended second is defined to be worth an awake idle second, and that identity survives a
// future retune of the curve only while both paths read the same expression.
static double thermalLoad(const Observation& s, const Constraints& c) {
    // An unreadable sensor is treated as fully hot, which drains everything and refills nothing.
    return s.thermalValid ? clip((s.temp - (c.high - 18)) / 18, 0, 1) : 1;
}
static double allowanceRate(double effort, double heat) {
    const double drain = effort * heat;
    const double refill = .5 * (1 - heat) * (1 - effort) + .25 * (1 - heat);
    return (refill - drain) / 300.;
}
void Budget::restore(double value) { level = std::isfinite(value) ? clip(value, 0, 1) : 1; }
void Budget::update(Action applied, const Observation& s, const Constraints& c, double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return;
    seconds = std::min(seconds, 60.);
    const double effort = applied.effort() / double(MaxEffort);
    // Effort is free while the device is far from its limit; it only draws down the allowance
    // once the same effort is what keeps the phone warm.
    const double heat = thermalLoad(s, c);
    // The allowance stays profile-blind on purpose. Draining it faster for Economia was tried as
    // a way to replace the per-profile cap that `safe()` used to apply -- a price instead of a
    // fence -- and it changed nothing in any scenario the simulation can produce, hot ones
    // included: the allowance only binds where high effort meets a hot device, and by then the
    // reward has already priced Economia down to almost no effort. A knob that moves no
    // measurement does not belong here. What restrains a profile is what it pays, in `costs`.
    level = clip(level + allowanceRate(effort, heat) * seconds, 0, 1);
}
void Budget::relax(const Observation& s, const Constraints& c, double seconds) {
    if (!std::isfinite(seconds) || seconds <= 0) return;
    // Long enough to fill an empty allowance several times over. Past that the number is more
    // likely a clock the kernel stepped than a sleep, and a full tank is already the answer.
    seconds = std::min(seconds, 3600.);
    // Zero effort through the same rate update() uses, so a suspended second and an awake idle
    // second are worth exactly the same -- which is the whole point. Nothing ran, so there is no
    // effort to charge and this can only add.
    level = clip(level + allowanceRate(0, thermalLoad(s, c)) * seconds, 0, 1);
}
int Budget::ceiling() const {
    return level >= .6 ? 4 : level >= .35 ? 3 : level >= .15 ? 2 : level > 0 ? 1 : 0;
}

double Critic::component(const Features& f, int term) const {
    const auto& w = weights[static_cast<size_t>(term)];
    double v = 0;
    for (size_t i = 0; i < Dim; ++i) v += w[i] * f[i];
    // Bounded, but NOT floored at zero. A discounted sum of non-negative costs cannot really be
    // negative, yet forcing that on a linear approximator buys nothing and costs the exact
    // linearity that lets every profile's value be read off the same weights -- which is the one
    // property this representation exists for. A negative prediction is prediction error, and TD
    // is already the thing that removes it.
    return std::isfinite(v) ? clip(v, -2 * ValueLimit, 2 * ValueLimit) : 0;
}
double Critic::value(const Features& f, Tier tier) const {
    const auto& w = preference(tier);
    double total = 0;
    for (int i = 0; i < Costs; ++i) total += w[i] * component(f, i);
    const double v = ValueLimit - 2 * total;
    return std::isfinite(v) ? clip(v, -ValueLimit, ValueLimit) : 0;
}
double Critic::value(const Observation& s, const Constraints& c, Action applied) const {
    return value(features(s, c, applied), c.tier);
}
double Critic::trust() const { return updates / (updates + 200.); }
double Critic::learn(const Features& before, const CostVector& measured, const Features& after,
                     Tier tier, double rate, double step) {
    for (double x : measured) if (!std::isfinite(x)) return 0;
    if (!std::isfinite(step)) return 0;
    const double discount = clip(step, 0, Discount);
    const double pace = clip(rate, 0, 1) * .02 / (1 + updates / 4000.);
    const double beta = std::min(.995, 1 - 1. / (updates + 2.));
    const auto& w = preference(tier);
    double advantage = 0;
    for (int term = 0; term < Costs; ++term) {
        const size_t t = static_cast<size_t>(term);
        // Each term is its own TD(0) problem against its own measured cost. They share the
        // feature vector and nothing else, so a window that says little about energy can still
        // say a great deal about lateness.
        const double error = clip(clip(measured[t], 0, 4) + discount * component(after, term) -
                                  component(before, term), -2, 2);
        // Per-feature step normalization. Frame lateness and jank vary over a far smaller range
        // than load or battery, and plain LMS would need tens of thousands of windows to weigh
        // them; this device supplies a few hundred. The direction is clipped so a single window
        // can never move a weight by more than MaxStep, whatever the gradient scale has become.
        for (size_t k = 0; k < Dim; ++k) {
            const double gradient = error * before[k];
            auto& variance = scale[t][k];
            // Floor the normalizer where it is already meaningless: it only ever divides
            // as sqrt(variance) + 1e-3, so anything below 1e-12 is indistinguishable from
            // zero downstream — except in the persisted text, where a decayed-to-denormal
            // scale ("4.89e-322") is unparseable on some libc implementations and used to
            // reject an entire valid brain on load. The floor keeps future files clean;
            // parseScaleToken recovers the ones already written.
            variance = std::max(1e-12, beta * variance + (1 - beta) * gradient * gradient);
            const double direction = clip(gradient / (std::sqrt(variance) + 1e-3), -2, 2);
            weights[t][k] = clip(weights[t][k] + pace * direction, -8, 8);
        }
        // The scalar advantage the policy consumes is the TD error of THIS profile's value,
        // which for V = ValueLimit - 2*(w . psi) is exactly -2 times the weighted term errors.
        advantage += w[t] * error;
    }
    if (rate >= 1 && updates < 1000000000U) ++updates;
    return clip(-2 * advantage, -2, 2);
}

std::vector<double> Prior::distribution(const std::string& coarse, Tier t,
                                        const std::vector<Choice>& options) const {
    std::vector<double> p(options.size(), 0);
    if (options.empty()) return p;
    const auto it = contexts.find(coarse);
    const auto& base = tier[static_cast<size_t>(t)];
    double top = -1e30;
    for (size_t i = 0; i < options.size(); ++i) {
        const auto move = static_cast<size_t>(options[i].move);
        p[i] = base[move] + (it != contexts.end() ? it->second.logit[move] : 0.f);
        top = std::max(top, p[i]);
    }
    double total = 0;
    for (auto& x : p) { x = std::exp(clip(x - top, -30, 0)); total += x; }
    const double uniform = 1. / static_cast<double>(options.size());
    for (auto& x : p) x = (1 - Floor) * (total > 0 ? x / total : uniform) + Floor * uniform;
    return p;
}
void Prior::reinforce(const std::string& coarse, Tier t, int move,
                      const std::vector<Choice>& options, double advantage) {
    if (options.empty() || !std::isfinite(advantage)) return;
    size_t taken = options.size();
    for (size_t i = 0; i < options.size(); ++i) if (options[i].move == move) { taken = i; break; }
    if (taken == options.size()) return;
    const auto p = distribution(coarse, t, options);
    if (!contexts.count(coarse) && contexts.size() >= MaxContexts) {
        auto oldest = std::min_element(contexts.begin(), contexts.end(),
            [](const auto& a, const auto& b) { return a.second.touched < b.second.touched; });
        contexts.erase(oldest);
    }
    auto& cell = contexts[coarse];
    cell.touched = ++updates;
    // Clipped policy gradient: one measured window nudges the preference, never rewrites it.
    const double signal = clip(advantage, -1, 1);
    auto& base = tier[static_cast<size_t>(t)];
    for (size_t i = 0; i < options.size(); ++i) {
        const auto move_i = static_cast<size_t>(options[i].move);
        const double grad = ((i == taken ? 1. : 0.) - p[i]) * signal;
        base[move_i] = static_cast<float>(clip(base[move_i] + .008 * grad, -1.5, 1.5));
        cell.logit[move_i] = static_cast<float>(clip(cell.logit[move_i] + .03 * grad, -1.5, 1.5));
    }
}

static std::string edge(const std::string& key, Action from, Action to) {
    return key + '/' + std::to_string(from.id()) + '/' + std::to_string(to.id());
}
static Metrics metrics(const Observation& s) { return {s.p95, s.jank, s.energy, s.temp, s.cpuPsi}; }
static Observation heuristic(const Observation& s, Action from, Action to) {
    auto p = s;
    double dc = to.level[0] - from.level[0], dg = to.level[1] - from.level[1];
    double dm = to.level[2] - from.level[2], dp = to.level[3] - from.level[3];
    // Asymmetric cold start. The cost side is physics this device cannot escape: more clock is
    // more power and more heat, so it is asserted at full strength. The benefit side is only a
    // hypothesis about this workload, so it is asserted weakly and left for measurement to
    // establish. Starting out confident that raising a floor helps is what makes a tuner park
    // every axis at maximum and call it tuning.
    double cpuBound = clip((s.cpu - .25) / .65 + s.cpuPsi, 0, 1);
    double gpuBound = clip((s.gpu - .35) / .6, 0, 1);
    double memBound = clip(std::max(s.cpu, s.gpu) * .5 + s.memPsi, 0, 1);
    double gain = .022 * dc * cpuBound + .026 * dg * gpuBound + .010 * dm * memBound + .007 * dp * cpuBound;
    p.p95 = s.p95 * (1 - gain);
    p.jank = s.jank * (1 - 1.4 * gain);
    p.cpuPsi = clip(s.cpuPsi - dc * .02, 0, 1);
    p.energy = clip(s.energy + .045 * dc + .055 * dg + .03 * dm + .015 * dp, 0, 1.5);
    p.temp = s.temp + s.trend * 6 + .22 * dc + .28 * dg + .12 * dm;
    p.trend = (p.temp - s.temp) / 6;
    p.at += 6;
    return p;
}
// A cell's evidence decays with age, in samples -- not in windows and not in wall-clock time,
// since `touched` stamps the global sample counter. There are TWO half-lives because the decay
// was doing two jobs that want opposite answers, and running both on one constant meant one of
// them was always wrong.
//
//   Prediction asks "how much of this residual do I believe?". That wants a long memory: the
//   measurement does not stop being true because the phone was used for something else since.
//
//   The novelty budget asks "is it time to re-test this alternative?". That wants a short clock,
//   and deliberately forgets: a zero-action state may look identical before and after a hardware
//   intervention becomes useful again, so prediction-error detection alone can never discover
//   that counterfactual. Something has to re-ask, without manufacturing data.
//
// One constant of 64 served the second job and destroyed the first. Measured on the saved brain
// after a week (8989 samples, 3072 cells at the cap): median cell staleness was 1730 samples, so
// the median cell had decayed by 2^-27. Of 3072 cells, 55 still held one effective observation
// and 9 held four -- and all 9 of those were self-edges (0->0, 25->25). Not one transition edge
// survived. predict(), whose trust weight is evidence/(evidence+4), had therefore been running
// on the cold-start heuristic alone with a week of measurements sitting in the file unable to
// reach it. At 1024 the same brain keeps 267 cells at one observation and 45 at four.
//
// Raising the revisit clock with it was tried and reverted the same day: tests/engine_sim.cpp's
// phase test, where a boost stops paying and later starts paying again, recovered to effort 0.40
// against 0.79 before. Nothing else can notice that recovery -- the engine is not boosting, so
// it never observes the edge that improved -- which is precisely the deadlock the short clock
// exists to break. So it keeps its own constant, unchanged.
constexpr double EvidenceHalfLife = 1024.;
constexpr double RevisitHalfLife = 64.;
static double decayed(const Experience& e, uint64_t samples, double halfLife) {
    const double age = samples > e.touched ? static_cast<double>(samples - e.touched) : 0;
    return e.count * std::exp2(-age / halfLife);
}
static double freshEvidence(const Experience& e, uint64_t samples) {
    return decayed(e, samples, EvidenceHalfLife);
}
// How much this model knows about one edge, at the FINE resolution and deliberately not at the
// coarse one -- even though predict() leans on the coarse parent through the backoff weight.
//
// This is the novelty budget in accept() and the curiosity bonus in search, and reading the
// parent here was tried and reverted (2026-09-17). The argument for it was real: the fine key
// adds a temperature octave and load octiles on top of the app, so every one of those buckets
// carries its own untried budget for an edge the device measured a few degrees away, which is
// far more exploration than the constants in accept() read as. The simulator answered that the
// budget is load-bearing, and asymmetrically so -- releasing gets 8 tries where spending gets 3,
// because releasing is the recoverable direction and is how the controller comes back DOWN.
// Suppressing it per fine bucket cost the furnace device its thermal ceiling outright: effort
// rose from 1.95 to 2.54, the peak went from 71.4 C to 92.2 C and 873 of 900 windows breached,
// where the same run with this function unchanged breached none.
//
// So the per-bucket budget stays. A new thermal or load regime is entitled to re-ask what a
// floor buys there, and paying a few windows for that is cheaper than being unable to release.
//
// It reads RevisitHalfLife, not the evidence half-life predict() uses: this is a clock for when
// to look again, not a measure of what is known. See the two-half-life note above.
unsigned Model::count(const ContextKey& key, Action from, Action to) const {
    auto it = cells.find(edge(key.fine, from, to));
    return it == cells.end() ? 0 : static_cast<unsigned>(std::ceil(decayed(it->second, samples, RevisitHalfLife)));
}
Observation Model::predict(const ContextKey& key, const Observation& s, Action from,
                           Action to, std::mt19937* random) const {
    auto p = heuristic(s, from, to);
    auto v = metrics(p);
    const auto fine = cells.find(edge(key.fine, from, to));
    const auto coarse = cells.find(edge(key.coarse, from, to));
    const Experience* exact = fine == cells.end() ? nullptr : &fine->second;
    const Experience* parent = coarse == cells.end() ? nullptr : &coarse->second;
    // Denominator 4, not 6: this device yields a few hundred windows per session, so measured
    // evidence has to start outweighing the cold start well before a hundred samples.
    const double exactEvidence = exact ? freshEvidence(*exact, samples) : 0;
    const double parentEvidence = parent ? freshEvidence(*parent, samples) : 0;
    const double trustExact = exactEvidence / (exactEvidence + 4.);
    const double trustParent = parentEvidence / (parentEvidence + 4.);
    // Backoff: the coarse cell covers whatever confidence the fine cell has not earned yet.
    const double weightParent = (1 - trustExact) * trustParent;
    for (size_t i = 0; i < v.size(); ++i) {
        double residual = 0, spread = 0;
        if (exact) {
            residual += trustExact * exact->mean[i];
            spread += trustExact * std::max(0., exact->variance[i]) / (exactEvidence + 1);
        }
        if (parent) {
            residual += weightParent * parent->mean[i];
            spread += weightParent * std::max(0., parent->variance[i]) / (parentEvidence + 1);
        }
        if (random && spread > 0) residual += std::normal_distribution<double>(0, std::sqrt(spread))(*random);
        v[i] += residual;
    }
    p.p95 = clip(v[0], 1, 500); p.jank = clip(v[1], 0, 1); p.energy = clip(v[2], 0, 1.5);
    p.temp = clip(v[3], 0, 120); p.cpuPsi = clip(v[4], 0, 1);
    p.trend = (p.temp - s.temp) / 6;
    return p;
}
bool Model::observe(const ContextKey& key, const Observation& before, Action from,
                    Action to, const Observation& after) {
    if (!learnable(before, after)) return false;
    const auto expected = metrics(heuristic(before, from, to)), actual = metrics(after);
    // An app can change its response without changing package, load bucket or target cadence.
    // Previously a well-sampled edge stayed "known" forever: exploration would never retry
    // its alternatives even when holding the chosen setting suddenly stopped serving work.
    // Reopen the context on an observed prediction failure beyond both an absolute floor and
    // four residual standard deviations. Keep the measured means; reduce their authority.
    // No scenario label, no reward change, and no imagined sample is introduced here.
    const auto known = cells.find(edge(key.coarse, from, to));
    if (known != cells.end() && known->second.count >= 8) {
        const auto& cell = known->second;
        const double error = std::abs(actual[0] - expected[0] - cell.mean[0]);
        const double threshold = std::max(4., 4 * std::sqrt(std::max(0., cell.variance[0]) + 1.));
        if (error > threshold) {
            const auto coarsePrefix = key.coarse + '/';
            const auto finePrefix = key.coarse + '|';
            for (auto& [id, entry] : cells)
                if (id.compare(0, coarsePrefix.size(), coarsePrefix) == 0 ||
                    id.compare(0, finePrefix.size(), finePrefix) == 0)
                    entry.count = std::min(entry.count, 1u);
            ++surprises;
        }
    }
    ++samples;
    for (const auto& scope : {key.fine, key.coarse}) {
        const auto id = edge(scope, from, to);
        // A fine cell is a REFINEMENT of its coarse parent, so it may not be opened before the
        // parent has something to refine. Without this the first observation of any edge spent
        // two table slots to store one measurement twice, split across two keys neither of which
        // could ever reach the trust weight in predict(). Measured on the week-old brain: 3072
        // cells at the cap, 65% of them holding exactly one observation, 1792 of them fine --
        // and 74% of those fine cells belonged to a coarse parent that had fewer than four
        // observations of its own. Promotion alone frees 43% of the table, which is the
        // difference between a model that accumulates and one that evicts what it just learned.
        //
        // The test is `!cells.count(id)`: an already-promoted cell keeps being updated even if a
        // surprise later knocks its parent's count back down. Demoting mid-life would throw away
        // measured means to satisfy a bookkeeping rule.
        if (scope == key.fine && scope != key.coarse && !cells.count(id)) {
            const auto parent = cells.find(edge(key.coarse, from, to));
            if (parent == cells.end() || parent->second.count < FinePromotion) continue;
        }
        if (!cells.count(id) && cells.size() >= MaxEntries) {
            // Evict the least EVIDENCE, not the least recently touched. Recency alone discards
            // the one cell on the device with 56 measurements of YouTube at 60 because it was
            // last seen this morning, while keeping two thousand cells that hold a single
            // observation each and happen to be newer. Same decay the rest of the model reads,
            // so a cell is worth exactly what predict() would weight it at; `touched` only
            // breaks ties between cells that are equally worthless.
            auto weakest = std::min_element(cells.begin(), cells.end(),
                [this](const auto& a, const auto& b) {
                    const double ea = freshEvidence(a.second, samples);
                    const double eb = freshEvidence(b.second, samples);
                    if (ea != eb) return ea < eb;
                    return a.second.touched < b.second.touched;
                });
            cells.erase(weakest);
        }
        auto& e = cells[id];
        e.count = std::min(128U, e.count + 1);
        e.touched = samples;
        const double alpha = 1. / std::min(32U, e.count); // recency weighting for workload drift
        for (size_t i = 0; i < actual.size(); ++i) {
            const double error = actual[i] - expected[i] - e.mean[i];
            e.mean[i] += alpha * error;
            e.variance[i] = (1 - alpha) * (e.variance[i] + alpha * error * error);
        }
        if (scope == key.coarse) break; // identical keys must not be counted twice
    }
    return true;
}

bool accept(const Model& model, const ContextKey& key, const Observation& s, Action current,
            Action next, const Constraints& c, bool explore) {
    if (next == current) return false;
    const double advantage = reward(model.predict(key, s, current, next), c, next) -
                             reward(model.predict(key, s, current, current), c, current);
    const bool costlier = next.effort() > current.effort();
    // Game asks for frames, so it acts on thinner evidence when spending more; the other tiers
    // make a boost prove itself first. Releasing is cheap to be wrong about in every tier.
    const double toll = costlier ? (c.tier == Tier::Game ? .008 : .015) : .004;
    if (advantage >= toll) return true;
    // Novelty, with an asymmetric budget: spending more is tried a few times before the model
    // is trusted, releasing is tried more freely because it is the recoverable direction.
    // Nothing here overrides safe(), the allowance ceiling or the thermal guard.
    return explore && model.count(key, current, next) < (costlier ? 3u : 8u);
}

Ambition ambition(const Model& model, const ContextKey& key, const Observation& s,
                  Action current, const Constraints& c, bool explore) {
    Ambition out;
    // The same margin accept() applies, recomputed here rather than plumbed out of it: this must
    // report what the gate WOULD do without being able to change what it does.
    out.toll = c.tier == Tier::Game ? .008 : .015;
    const double stay = reward(model.predict(key, s, current, current), c, current);
    for (const auto& choice : candidates(current, s, c)) {
        const Action& next = choice.action;
        if (next == current || next.effort() <= current.effort()) continue;
        const double advantage = reward(model.predict(key, s, current, next), c, next) - stay;
        if (out.move >= 0 && advantage <= out.advantage) continue;
        out.move = choice.move;
        out.advantage = advantage;
        out.tried = model.count(key, current, next);
        out.accepted = accept(model, key, s, current, next, c, explore);
    }
    return out;
}

void Replay::add(const Features& before, const CostVector& measured, const Features& after,
                 Tier tier, double step, std::mt19937& rng) {
    Sample sample;
    sample.before = before; sample.after = after;
    for (int i = 0; i < Costs; ++i) sample.cost[i] = clip(measured[i], 0, 4);
    sample.step = std::isfinite(step) ? clip(step, 0, Discount) : Discount;
    sample.tier = static_cast<uint8_t>(tier);
    ++seen;
    if (samples.size() < Capacity) { samples.push_back(sample); return; }
    // Half reservoir, half recency: old lessons survive, the current workload stays represented.
    if (std::uniform_real_distribution<double>(0, 1)(rng) < .5)
        samples[std::uniform_int_distribution<size_t>(0, Capacity - 1)(rng)] = sample;
    else
        samples[seen % Capacity] = sample;
}
int Replay::rehearse(Critic& critic, std::mt19937& rng, int steps) {
    if (samples.empty() || steps <= 0) return 0;
    std::uniform_int_distribution<size_t> pick(0, samples.size() - 1);
    const int total = std::min(steps, 256);
    for (int i = 0; i < total; ++i) {
        const auto& sample = samples[pick(rng)];
        critic.learn(sample.before, sample.cost, sample.after,
                     static_cast<Tier>(std::min<uint8_t>(sample.tier, 2)), .35, sample.step);
    }
    return total;
}

// One stored key looks like `<coarse>[|buckets]/<from>/<to>` for a residual cell and
// `<tier>:<coarse>` for a policy cell, where `<coarse>` ends in the configuration identity.
// Version 3 additionally prefixed every residual key with the tier. `rehome` maps a stored
// configuration identity onto the current one; a key naming an identity absent from the map was
// measured on a tuning surface this build cannot reconstruct, and is dropped rather than guessed.
static bool rehomeKey(std::string& key, const std::map<std::string, std::string>& rehome,
                      bool stripTier) {
    if (rehome.empty()) return false;
    const auto colon = key.find(':');
    if (colon == std::string::npos || colon > 2) return false;
    std::string prefix = stripTier ? std::string() : key.substr(0, colon + 1);
    key.erase(0, colon + 1);
    // The configuration identity is the sixth colon-separated field of the coarse part.
    size_t at = 0;
    for (int i = 0; i < 5; ++i) {
        at = key.find(':', at);
        if (at == std::string::npos) return false;
        ++at;
    }
    const auto stop = key.find_first_of("|/", at);
    const auto id = key.substr(at, stop == std::string::npos ? std::string::npos : stop - at);
    const auto found = rehome.find(id);
    if (found == rehome.end()) return false;
    key = prefix + key.substr(0, at) + found->second +
        (stop == std::string::npos ? std::string() : key.substr(stop));
    return true;
}
// Seeds the per-term predictor from the per-tier value functions of an older brain. For every
// feature k the stored weights give one linear equation per tier that had evidence:
//
//     W_tier[k] = -2 * ( preference(tier) . psi[.][k] )      (the bias also absorbs ValueLimit)
//
// Two or three equations in nine unknowns is underdetermined. The minimum-norm answer is the
// textbook one and it is wrong here: the three objectives share their thermal, rising, battery
// and breach weights *exactly*, so their preference rows are nearly parallel, the Gram matrix is
// nearly singular, and the solution it produces is enormous and numerically meaningless.
//
// So solve exactly on the smallest set of cost terms that can carry the answer without that
// pathology: the subset of terms whose square submatrix is best conditioned, everything else
// left at zero. Every value function that was actually measured is reproduced exactly; the terms
// the data never distinguished start empty, which is what not knowing looks like. TD then
// redistributes as real measurements arrive. Discarding 1500 windows of fitted weights over a
// change of representation would have been a choice, not a necessity.
static void warmStart(Critic& critic, const std::array<std::array<double, Critic::Dim>, 3>& legacy,
                      const std::array<uint32_t, 3>& evidence) {
    std::vector<size_t> tiers;
    for (size_t t = 0; t < 3; ++t) if (evidence[t] > 0) tiers.push_back(t);
    if (tiers.empty()) return;
    const size_t m = tiers.size();
    auto row = [&](size_t a) -> const CostVector& { return preference(static_cast<Tier>(tiers[a])); };
    auto determinant = [&](const std::vector<double>& a, size_t n) {
        std::vector<double> w(a);
        double det = 1;
        for (size_t col = 0; col < n; ++col) {
            size_t pivot = col;
            for (size_t r = col + 1; r < n; ++r)
                if (std::abs(w[r * n + col]) > std::abs(w[pivot * n + col])) pivot = r;
            if (std::abs(w[pivot * n + col]) < 1e-12) return 0.;
            if (pivot != col) { det = -det; for (size_t c = 0; c < n; ++c) std::swap(w[col * n + c], w[pivot * n + c]); }
            det *= w[col * n + col];
            for (size_t r = col + 1; r < n; ++r) {
                const double f = w[r * n + col] / w[col * n + col];
                for (size_t c = col; c < n; ++c) w[r * n + c] -= f * w[col * n + c];
            }
        }
        return det;
    };
    // Brute force over every subset of size m; there are at most 84 of them.
    std::vector<int> chosen;
    double best = 0;
    std::vector<int> subset(m, 0);
    std::function<void(int, size_t)> walk = [&](int start, size_t depth) {
        if (depth == m) {
            std::vector<double> square(m * m, 0);
            for (size_t a = 0; a < m; ++a)
                for (size_t j = 0; j < m; ++j) square[a * m + j] = row(a)[subset[j]];
            const double d = std::abs(determinant(square, m));
            if (d > best) { best = d; chosen = subset; }
            return;
        }
        for (int i = start; i < Costs; ++i) { subset[depth] = i; walk(i + 1, depth + 1); }
    };
    walk(0, 0);
    if (chosen.empty() || best < 1e-4) return; // nothing well conditioned: leave the critic cold
    // Invert the chosen square block once.
    std::vector<double> work(m * m, 0), inverse(m * m, 0);
    for (size_t a = 0; a < m; ++a) {
        inverse[a * m + a] = 1;
        for (size_t j = 0; j < m; ++j) work[a * m + j] = row(a)[chosen[j]];
    }
    for (size_t col = 0; col < m; ++col) {
        size_t pivot = col;
        for (size_t r = col + 1; r < m; ++r)
            if (std::abs(work[r * m + col]) > std::abs(work[pivot * m + col])) pivot = r;
        if (std::abs(work[pivot * m + col]) < 1e-12) return;
        for (size_t c = 0; c < m; ++c) {
            std::swap(work[col * m + c], work[pivot * m + c]);
            std::swap(inverse[col * m + c], inverse[pivot * m + c]);
        }
        const double d = work[col * m + col];
        for (size_t c = 0; c < m; ++c) { work[col * m + c] /= d; inverse[col * m + c] /= d; }
        for (size_t r = 0; r < m; ++r) {
            if (r == col) continue;
            const double factor = work[r * m + col];
            if (factor == 0) continue;
            for (size_t c = 0; c < m; ++c) {
                work[r * m + c] -= factor * work[col * m + c];
                inverse[r * m + c] -= factor * inverse[col * m + c];
            }
        }
    }
    for (size_t k = 0; k < Critic::Dim; ++k) {
        std::vector<double> target(m, 0), solved(m, 0);
        for (size_t a = 0; a < m; ++a)
            target[a] = ((k == 0 ? ValueLimit : 0) - legacy[tiers[a]][k]) / 2;
        for (size_t j = 0; j < m; ++j)
            for (size_t a = 0; a < m; ++a) solved[j] += inverse[j * m + a] * target[a];
        for (size_t j = 0; j < m; ++j)
            critic.weights[static_cast<size_t>(chosen[j])][k] = clip(solved[j], -8, 8);
    }
}

std::string Brain::serialize(const std::string& identity) const {
    std::ostringstream out;
    out << "M54_BRAIN_5 " << identity << ' ' << model.samples << ' ' << windows << ' '
        << prior.updates << ' ' << replay.seen << '\n' << std::setprecision(9);
    out << "B " << budget << '\n';
    for (const auto& [key, e] : model.cells) {
        out << "C " << key << ' ' << e.count << ' ' << e.touched;
        for (auto x : e.mean) out << ' ' << x;
        for (auto x : e.variance) out << ' ' << x;
        out << '\n';
    }
    out << "U " << critic.updates << '\n';
    for (size_t t = 0; t < static_cast<size_t>(Costs); ++t) {
        out << "V " << t;
        for (auto x : critic.weights[t]) out << ' ' << x;
        for (auto x : critic.scale[t]) out << ' ' << x;
        out << '\n';
    }
    for (size_t t = 0; t < 3; ++t) {
        out << "P " << t;
        for (auto x : prior.tier[t]) out << ' ' << x;
        out << '\n';
    }
    for (const auto& [key, cell] : prior.contexts) {
        out << "Q " << key << ' ' << cell.touched;
        for (auto x : cell.logit) out << ' ' << x;
        out << '\n';
    }
    out << std::setprecision(6);
    for (const auto& sample : replay.samples) {
        out << "R " << static_cast<int>(sample.tier) << ' ' << sample.step;
        for (auto x : sample.cost) out << ' ' << x;
        for (auto x : sample.before) out << ' ' << x;
        for (auto x : sample.after) out << ' ' << x;
        out << '\n';
    }
    const auto payload = out.str();
    return payload + "CHECK " + std::to_string(hash(payload)) + '\n';
}
bool Brain::deserialize(const std::string& data, const std::string& identity,
                        const std::map<std::string, std::string>& rehome) {
    const auto check = data.rfind("CHECK ");
    if (check == std::string::npos || data.size() > 4 * 1024 * 1024) return false;
    std::istringstream tail(data.substr(check + 6));
    uint64_t checksum = 0;
    if (!(tail >> checksum) || checksum != hash(data.substr(0, check))) return false;
    std::istringstream in(data.substr(0, check));
    std::string version, id;
    uint64_t totalSamples = 0, totalWindows = 0, priorUpdates = 0, replaySeen = 0;
    if (!(in >> version >> id >> totalSamples >> totalWindows >> priorUpdates >> replaySeen) ||
        id != identity) return false;
    const bool legacy = version == "M54_BRAIN_3" || version == "M54_BRAIN_2";
    if (!legacy && version != "M54_BRAIN_4" && version != "M54_BRAIN_5") return false;
    // Version 2 predates the regime channels, versions 3 and 4 the paging channels. Older weights
    // are read at their own width and the features added since start at zero, which is where an
    // unseen feature starts anyway. Reading a narrow file at the current width would swallow the
    // next row's tokens and throw away a brain that is perfectly usable.
    const size_t dim = version == "M54_BRAIN_2" ? LegacyFeatures
                     : version == "M54_BRAIN_5" ? Critic::Dim
                                                : PagingFreeFeatures;
    Brain restored;
    std::array<std::array<double, Critic::Dim>, 3> legacyWeights{};
    std::array<uint32_t, 3> legacyUpdates{};
    auto bounded = [](double x, double limit) { return std::isfinite(x) && std::abs(x) <= limit; };
    std::string tag;
    while (in >> tag) {
        if (tag == "B") {
            double value = 0;
            if (!(in >> value) || !std::isfinite(value) || value < 0 || value > 1) return false;
            restored.budget = value;
        } else if (tag == "C") {
            std::string key; Experience e;
            if (!(in >> key >> e.count >> e.touched) || key.size() > 320 || e.count < 1 ||
                e.count > 128 || e.touched > totalSamples) return false;
            for (auto& x : e.mean) if (!(in >> x) || !bounded(x, 10000)) return false;
            for (auto& x : e.variance) if (!(in >> x) || !std::isfinite(x) || x < 0 || x > 1e8) return false;
            // An older residual key carries the tier it was measured under. Dropping that prefix
            // is what merges the two halves of one device's physics back together; where both
            // halves hold the same edge, the evidence is pooled instead of one of them winning.
            if (legacy && !rehomeKey(key, rehome, true)) continue;
            auto seat = restored.model.cells.find(key);
            if (seat == restored.model.cells.end()) {
                if (restored.model.cells.size() >= Model::MaxEntries) return false;
                restored.model.cells.emplace(key, e);
            } else {
                auto& live = seat->second;
                const double n1 = live.count, n2 = e.count, n = n1 + n2;
                for (size_t i = 0; i < live.mean.size(); ++i) {
                    const double mean = (n1 * live.mean[i] + n2 * e.mean[i]) / n;
                    const double second = (n1 * (live.variance[i] + live.mean[i] * live.mean[i]) +
                                           n2 * (e.variance[i] + e.mean[i] * e.mean[i])) / n;
                    live.mean[i] = mean;
                    live.variance[i] = std::max(0., second - mean * mean);
                }
                live.count = static_cast<unsigned>(std::min(128., n));
                live.touched = std::max(live.touched, e.touched);
            }
        } else if (tag == "U") {
            if (!(in >> restored.critic.updates)) return false;
        } else if (tag == "V") {
            size_t t = 0;
            if (!(in >> t)) return false;
            if (legacy) {
                if (t > 2 || !(in >> legacyUpdates[t])) return false;
                for (size_t k = 0; k < dim; ++k)
                    if (!(in >> legacyWeights[t][k]) || !bounded(legacyWeights[t][k], 8)) return false;
                double discard = 0;
                for (size_t k = 0; k < dim; ++k) {
                    std::string tok;
                    if (!(in >> tok) || !parseScaleToken(tok, discard) || discard < 0 ||
                        discard > 100)
                        return false;
                }
            } else {
                if (t >= static_cast<size_t>(Costs)) return false;
                for (size_t k = 0; k < dim; ++k) {
                    auto& x = restored.critic.weights[t][k];
                    if (!(in >> x) || !bounded(x, 8)) return false;
                }
                for (size_t k = 0; k < dim; ++k) {
                    std::string tok;
                    auto& x = restored.critic.scale[t][k];
                    if (!(in >> tok) || !parseScaleToken(tok, x) || x < 0 || x > 100)
                        return false;
                }
            }
        } else if (tag == "P") {
            size_t t = 0;
            if (!(in >> t) || t > 2) return false;
            for (auto& x : restored.prior.tier[t]) if (!(in >> x) || !bounded(x, 1.5)) return false;
        } else if (tag == "Q") {
            std::string key; Prior::Cell cell;
            if (!(in >> key >> cell.touched) || key.size() > 320 || cell.touched > priorUpdates) return false;
            for (auto& x : cell.logit) if (!(in >> x) || !bounded(x, 1.5)) return false;
            // A policy key keeps its tier: preference over moves is the one thing that
            // legitimately differs between objectives. Only the identity inside it is rehomed.
            if (legacy && !rehomeKey(key, rehome, false)) continue;
            if (!restored.prior.contexts.emplace(key, cell).second ||
                restored.prior.contexts.size() > Prior::MaxContexts) return false;
        } else if (tag == "R") {
            int t = 0; Replay::Sample sample; double gain = 0;
            if (!(in >> t) || t < 0 || t > 2) return false;
            if (legacy) { if (!(in >> gain >> sample.step) || !bounded(gain, 2)) return false; }
            else {
                if (!(in >> sample.step)) return false;
                for (auto& x : sample.cost)
                    if (!(in >> x) || !std::isfinite(x) || x < 0 || x > 4) return false;
            }
            if (!(sample.step >= 0 && sample.step <= Discount)) return false;
            sample.tier = static_cast<uint8_t>(t);
            for (size_t k = 0; k < dim; ++k)
                if (!(in >> sample.before[k]) || !bounded(sample.before[k], 1)) return false;
            for (size_t k = 0; k < dim; ++k)
                if (!(in >> sample.after[k]) || !bounded(sample.after[k], 1)) return false;
            if (restored.replay.samples.size() >= Replay::Capacity) return false;
            // A stored window from an older format is parsed and dropped. It holds one scalar
            // return under one profile, and the per-term costs behind it are not recoverable from
            // that number; inventing them would be inventing measurements. The buffer refills
            // from live windows in about half an hour of use.
            if (!legacy) restored.replay.samples.push_back(sample);
        } else return false;
    }
    if (legacy) {
        warmStart(restored.critic, legacyWeights, legacyUpdates);
        // The seeded weights reproduce every value function that was measured, but the directions
        // no measured profile constrained are a minimum-norm guess. Counting those as experience
        // would report a confidence nothing earned, so the evidence counter starts from zero
        // while the weights it starts from do not.
        restored.critic.updates = 0;
    }
    restored.model.samples = totalSamples;
    restored.windows = totalWindows;
    restored.prior.updates = priorUpdates;
    restored.replay.seen = restored.replay.samples.empty() ? 0 : replaySeen;
    *this = std::move(restored);
    return true;
}

Decision Planner::search(const Brain& brain, const ContextKey& key, const Observation& s,
                         Action current, const Constraints& limits, bool explore,
                         int iterations, int horizon, int budgetMicros) {
    using Clock = std::chrono::steady_clock;
    const auto deadline = Clock::now() + std::chrono::microseconds(std::clamp(budgetMicros, 100, 20000));
    horizon = std::clamp(horizon, 2, 6);
    iterations = std::clamp(iterations, 1, 512);
    const auto& model = brain.model;
    struct Node {
        Observation state;
        Action action;
        int move = Stay, parent = -1, depth = 0, visits = 0;
        double sum = 0, immediate = 0, prior = 0;
        std::vector<int> children;
        std::vector<Choice> pending;
        std::vector<double> pendingPrior;
    };
    current = safe(current, s, limits);
    // Optimism in the face of uncertainty, only while the device has headroom to spare
    // (MBIE-EB style, decaying with measured evidence). It biases what the search is willing
    // to *propose*; it never enters a TD target, so learning stays grounded in what was
    // measured. Without it the search can only ever re-derive its own cold start.
    const double curiosity = explore ? .06 : 0;
    auto bonus = [&](Action from, Action to) {
        return curiosity / std::sqrt(1. + model.count(key, from, to));
    };
    std::vector<Node> tree;
    tree.reserve(static_cast<size_t>(iterations) + 1);
    {
        Node root;
        root.state = s; root.action = current;
        root.pending = candidates(current, s, limits);
        root.pendingPrior = brain.prior.distribution(key.policy, limits.tier, root.pending);
        if (explore && root.pending.size() > 1) {
            // Root-only Dirichlet noise: exploration is injected where its effect can actually
            // be measured on the device, never deep inside the imagined tree.
            std::gamma_distribution<double> gamma(.6, 1);
            std::vector<double> noise(root.pendingPrior.size());
            double total = 0;
            for (auto& x : noise) { x = gamma(rng); total += x; }
            if (total > 0)
                for (size_t i = 0; i < noise.size(); ++i)
                    root.pendingPrior[i] = .75 * root.pendingPrior[i] + .25 * noise[i] / total;
        }
        tree.push_back(std::move(root));
    }
    Decision result;
    result.action = current;
    for (int simulation = 0; simulation < iterations && Clock::now() < deadline; ++simulation) {
        int selected = 0;
        // Selection: PUCT, so an untried move with a strong learned prior is reached early.
        while (tree[selected].depth < horizon && tree[selected].pending.empty() &&
               !tree[selected].children.empty()) {
            int best = -1;
            double bestScore = -1e30;
            const double parentVisits = std::sqrt(std::max(1., double(tree[selected].visits)));
            for (int child : tree[selected].children) {
                const auto& n = tree[child];
                const double q = n.visits ? n.sum / n.visits / ValueLimit : 0;
                const double score = q + 1.25 * n.prior * parentVisits / (1. + n.visits);
                if (score > bestScore) { bestScore = score; best = child; }
            }
            if (best < 0) break;
            selected = best;
        }
        if (tree[selected].depth < horizon && !tree[selected].pending.empty()) {
            size_t pick = 0;
            for (size_t i = 1; i < tree[selected].pending.size(); ++i)
                if (tree[selected].pendingPrior[i] > tree[selected].pendingPrior[pick]) pick = i;
            const auto choice = tree[selected].pending[pick];
            const double chosenPrior = tree[selected].pendingPrior[pick];
            tree[selected].pending.erase(tree[selected].pending.begin() + static_cast<long>(pick));
            tree[selected].pendingPrior.erase(tree[selected].pendingPrior.begin() + static_cast<long>(pick));
            const auto parentAction = tree[selected].action;
            auto next = model.predict(key, tree[selected].state, parentAction, choice.action, &rng);
            // Predictive pruning: never grow a branch whose own model says it crosses the limit.
            const bool risky = choice.action.effort() > parentAction.effort() &&
                (next.temp >= limits.high - 1 || next.batteryTemp >= limits.batteryHigh - .5);
            if (!risky) {
                Node child;
                child.state = next;
                child.action = choice.action;
                child.move = choice.move;
                child.parent = selected;
                child.depth = tree[selected].depth + 1;
                child.prior = chosenPrior;
                child.immediate = reward(next, limits, choice.action) +
                    bonus(parentAction, choice.action) -
                    (choice.action != parentAction ? .015 : 0);
                child.pending = candidates(choice.action, next, limits);
                child.pendingPrior = brain.prior.distribution(key.policy, limits.tier, child.pending);
                const int index = static_cast<int>(tree.size());
                tree.push_back(std::move(child));
                tree[selected].children.push_back(index);
                selected = index;
                result.depth = std::max(result.depth, tree[index].depth);
            }
        }
        // Rollout under the learned prior, then bootstrap the tail with the critic. Early on
        // the critic is zero and this is a plain truncated rollout; as it learns, the tail
        // carries the long-horizon consequence that four imagined steps cannot reach.
        auto state = tree[selected].state;
        auto action = tree[selected].action;
        double value = 0, discount = 1;
        for (int depth = tree[selected].depth; depth < horizon; ++depth) {
            const auto options = candidates(action, state, limits);
            const auto priors = brain.prior.distribution(key.policy, limits.tier, options);
            std::discrete_distribution<size_t> sample(priors.begin(), priors.end());
            const auto choice = options[sample(rng)];
            state = model.predict(key, state, action, choice.action, &rng);
            value += discount * (reward(state, limits, choice.action) +
                                 bonus(action, choice.action) -
                                 (choice.action != action ? .015 : 0));
            discount *= Discount;
            action = choice.action;
        }
        value += discount * brain.critic.value(state, limits, action);
        // Backpropagation updates the SEARCH tree only; it does not manufacture experience.
        while (selected >= 0) {
            auto& n = tree[selected];
            value = n.immediate + Discount * value;
            ++n.visits;
            n.sum += value;
            selected = n.parent;
        }
        ++result.simulations;
    }
    int best = -1;
    for (int child : tree[0].children) {
        if (best < 0) { best = child; continue; }
        const auto& a = tree[child];
        const auto& b = tree[best];
        if (a.visits > b.visits || (a.visits == b.visits &&
            a.sum / std::max(1, a.visits) > b.sum / std::max(1, b.visits))) best = child;
    }
    if (best >= 0) {
        result.action = safe(tree[best].action, s, limits);
        result.move = tree[best].move;
        result.value = tree[best].sum / std::max(1, tree[best].visits);
        result.prior = tree[best].prior;
    }
    result.nodes = static_cast<int>(tree.size());
    return result;
}
} // namespace m54
