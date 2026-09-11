// Closed-loop regression test. The unit tests prove each part is wired correctly; this one
// proves the assembled controller reaches the right answer when the right answer is known.
//
// The synthetic device is deliberately simple, and passing here is NOT evidence of a gain on
// the real M54: it only shows the loop converges toward the objective it was given, refuses to
// spend where nothing is bought, and respects its thermal limit under a hostile heat curve.
#include "../module/engine/core.hpp"
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <random>

using namespace m54;

struct Device {
    bool responsive;   // does raising a floor actually cut frame latency here?
    double noise;
    double heatGain;   // degrees above idle at full effort
    double temp = 38, previous = 38;
    std::mt19937 rng{11};
    Observation step(Action a, int target, double at) {
        // NOTE: heat and energy deliberately follow the charged effort(), not the hardware
        // effect — PELT 1 heats here although it writes the same 2x baseline as 0 on the
        // real node. Decoupling only this side breaches the furnace (411 breaches, peak
        // 82.9 C, against 0 at 72.7 C coupled), because pricing and physics must move
        // together: the controller's phantom heat charge currently coincides with phantom
        // sim heat. Any PELT repricing changes this line and Action::effort() in the same
        // commit, validated on device — never one side alone.
        const double effort = a.effort() / 16.;
        temp += ((36 + heatGain * effort) - temp) * .18;
        double base = 26.;
        if (responsive) base *= (1 - .16 * a.level[1] - .04 * a.level[0]);
        base += std::normal_distribution<double>(0, noise)(rng);
        Observation s;
        s.at = at; s.target = target;
        s.p95 = std::max(6., base);
        s.jank = std::clamp((s.p95 - 1000. / target) / (1000. / target), 0., 1.) * .6;
        s.cpu = .62 + .05 * a.level[0]; s.gpu = .80 + .03 * a.level[1];
        s.cpuPsi = .12; s.energy = .35 + .55 * effort;
        s.temp = temp; s.batteryTemp = 31 + 4 * effort;
        s.trend = (temp - previous) / 6; previous = temp;
        s.frames = 300; s.battery = 80;
        s.awake = s.thermalValid = s.framesValid = s.loadValid = true;
        s.app = "org.sim.game";
        return s;
    }
};
struct Outcome {
    double effort = 0, gpu = 0, p95 = 0, gain = 0, peak = 0;
    int breaches = 0;
    uint64_t windows = 0, samples = 0;
};

// Mirrors the daemon's window in main.cpp: measure, credit the previous window, plan, gate.
static Outcome run(Device device, Tier tier, int windows, uint32_t seed,
                   std::array<Outcome, 3>* phases = nullptr) {
    Brain brain; Planner planner(seed); Budget allowance;
    Constraints limits; limits.tier = tier; limits.high = 75;
    Action current, previous;
    Observation before; ContextKey beforeKey; Features beforeFeatures{};
    std::vector<Choice> beforeOptions; int beforeMove = Stay; bool eligible = false;
    Outcome out; double at = 0; int counted = 0;
    for (int w = 0; w < windows; ++w) {
        // A change in physical response, not a label supplied to the controller. Same app,
        // objective, target, memory and permitted actions across all three phases.
        const int phase = std::min(2, w / (windows / 3));
        if (phases) device.responsive = phase != 1;
        at += 6;
        auto s = device.step(current, 60, at);
        allowance.update(current, s, limits, 6);
        limits.ceiling = allowance.ceiling();
        auto key = context(s, limits, "sim");
        if (eligible && attributable(before, s)) {
            const auto measured = costs(s, limits, current);
            const auto after = features(s, limits, current);
            const double error = brain.critic.learn(beforeFeatures, measured, after, tier);
            brain.replay.add(beforeFeatures, measured, after, tier, Discount, planner.random());
            brain.prior.reinforce(beforeKey.policy, tier, beforeMove, beforeOptions, error);
            brain.model.observe(beforeKey, before, previous, current, s);
            ++brain.windows;
        }
        const bool explore = s.temp < limits.high - 10 && s.batteryTemp < 39 &&
            s.battery >= 25 && limits.ceiling >= 2;
        const auto decision = planner.search(brain, key, s, current, limits, explore);
        const auto next = accept(brain.model, key, s, current, decision.action, limits, explore)
            ? decision.action : current;
        beforeOptions = candidates(current, s, limits);
        beforeMove = moveIndex(current, next, s, limits);
        beforeFeatures = features(s, limits, current);
        before = s; beforeKey = key; eligible = beforeMove >= 0;
        previous = current; current = next;
        out.peak = std::max(out.peak, s.temp);
        if (s.temp >= limits.high) ++out.breaches;
        if (w >= windows / 2) { // judge the settled half, not the learning half
            out.effort += current.effort(); out.gpu += current.level[1];
            out.p95 += s.p95; out.gain += reward(s, limits, current); ++counted;
        }
        if (phases && w % (windows / 3) >= windows / 6) {
            auto& o = (*phases)[phase];
            o.effort += current.effort(); o.gpu += current.level[1];
            o.p95 += s.p95; o.gain += reward(s, limits, current);
            o.peak = std::max(o.peak, s.temp);
            if (s.temp >= limits.high) ++o.breaches;
            ++o.windows;
        }
    }
    out.effort /= counted; out.gpu /= counted; out.p95 /= counted; out.gain /= counted;
    out.windows = brain.windows; out.samples = brain.model.samples;
    if (phases) for (auto& o : *phases) {
        o.effort /= o.windows; o.gpu /= o.windows; o.p95 /= o.windows; o.gain /= o.windows;
    }
    return out;
}
static void report(const char* label, const Outcome& o) {
    printf("  %-16s effort=%4.2f gpu=%4.2f p95=%5.1fms reward=%+.3f peak=%4.1fC over=%d windows=%llu\n",
           label, o.effort, o.gpu, o.p95, o.gain, o.peak, o.breaches,
           static_cast<unsigned long long>(o.windows));
}
// One seed is a coin flip, not a result. Measured while removing the per-profile cap: at seed 5
// alone, Economia and Balanceado swapped operating points entirely -- 2.0/17.7 ms against
// 1.0/21.8 ms -- and either one, read by itself, would have looked like a regression or a win.
// Across seeds the ordering they are supposed to have is stable. Every assertion below is on the
// average, so it tests the controller instead of the draw.
static Outcome average(Device device, Tier tier, int windows) {
    static const uint32_t seeds[] = {5, 11, 23, 37, 101, 211, 307};
    Outcome mean;
    const double n = sizeof(seeds) / sizeof(seeds[0]);
    for (uint32_t seed : seeds) {
        const auto o = run(device, tier, windows, seed);
        mean.effort += o.effort / n; mean.gpu += o.gpu / n; mean.p95 += o.p95 / n;
        mean.gain += o.gain / n;
        mean.peak = std::max(mean.peak, o.peak);      // the worst seed, not the average seed
        mean.breaches += o.breaches;                  // any breach by any seed is a breach
        mean.windows += o.windows;
        mean.samples += o.samples;
    }
    mean.windows = static_cast<uint64_t>(mean.windows / n);
    return mean;
}
int main() {
    const int windows = 900; // 90 minutes of play at one 6 s window each
    Device responsive{true, .8, 26}, flat{false, .8, 26}, furnace{true, .8, 150};
    auto game = average(responsive, Tier::Game, windows);
    auto nothing = average(flat, Tier::Game, windows);
    auto hot = average(furnace, Tier::Game, windows);
    auto economy = average(responsive, Tier::Powersave, windows);
    auto balanced = average(responsive, Tier::Balanced, windows);
    printf("closed loop over %d windows of 6 s, averaged over 7 seeds:\n", windows);
    report("game/responsive", game);
    report("game/flat", nothing);
    report("game/runs-hot", hot);
    report("powersave/resp", economy);
    report("balanced/resp", balanced);

    // Learning must actually accrue, from every stable window and not only applied changes.
    for (const auto& o : {game, nothing, hot, economy, balanced})
        assert(o.windows > static_cast<uint64_t>(windows) * 9 / 10 && o.samples > 0);

    // Where a floor genuinely buys frame time, it is bought and the target budget is met.
    // Continued re-exploration pays a small stationary cost; the contract is the actual 60 Hz
    // budget, not the old 15 ms over-performance margin of a policy that never revisited edges.
    assert(game.gpu >= 2 && game.p95 < 1000. / 60);
    // Where it buys nothing, nothing is spent. This is the failure mode of every "tuner"
    // that assumes more clock is more performance.
    assert(nothing.effort < .5 && nothing.p95 > 20);
    assert(nothing.gain > average(flat, Tier::Game, 40).gain); // holding still beats flailing
    // On a device that heats six times faster the controller caps itself, and the limit holds.
    assert(hot.breaches == 0 && hot.peak < 75 && hot.gpu >= 1 && hot.gpu < game.gpu);
    for (const auto& o : {game, nothing, economy, balanced}) assert(o.breaches == 0 && o.peak < 75);
    // The tiers are different objectives, not three labels over one setting -- and they are that
    // now through what they PAY, since the per-profile ceiling in safe() is gone.
    assert(economy.effort < balanced.effort && balanced.effort < game.effort);
    // Economia still buys fluidity when it is cheap enough to be worth it: clearly better than
    // spending nothing at all, and clearly less than Game. The bound is 24 ms rather than the 22
    // it was under the ceiling: removing that fence cost Economia about 0.6 ms of p95 and 2% of
    // its own objective in this benchmark, measured by restoring the fence and rerunning. That is
    // the price of letting it find out for itself whether the level it was forbidden was worth
    // buying -- a question a stationary synthetic device never asks and a real one asks daily.
    assert(economy.p95 < 24 && economy.p95 < nothing.p95 - 2);
    printf("engine_sim: convergence, refusal to overspend, tier separation and thermal ceiling passed\n");

    std::array<Outcome, 3> changing{};
    for (uint32_t seed : {5, 11, 23, 37, 101, 211, 307}) {
        std::array<Outcome, 3> phases{};
        const auto o = run(responsive, RuntimeObjective, 2700, seed, &phases);
        assert(o.windows >= 2690 && o.samples > 0 && o.breaches == 0);
        for (size_t i = 0; i < phases.size(); ++i) {
            changing[i].effort += phases[i].effort / 7;
            changing[i].gpu += phases[i].gpu / 7;
            changing[i].p95 += phases[i].p95 / 7;
            changing[i].gain += phases[i].gain / 7;
            changing[i].peak = std::max(changing[i].peak, phases[i].peak);
            changing[i].windows += phases[i].windows;
        }
    }
    for (auto& o : changing) o.windows /= 7;
    report("auto/benefit", changing[0]);
    report("auto/no-benefit", changing[1]);
    report("auto/benefit-back", changing[2]);
    // Do not pass merely by emitting different labels: actual effort must follow measured
    // usefulness, and fluency must recover when effort becomes useful again.
    assert(changing[0].effort > changing[1].effort + .5);
    assert(changing[2].effort > changing[1].effort + .5);
    assert(changing[0].p95 < changing[1].p95 - 2);
    assert(changing[2].p95 < changing[1].p95 - 2);
    printf("engine_sim: automatic adaptation to unannounced workload changes passed\n");
}
