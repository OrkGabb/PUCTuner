#include "../module/engine/platform.hpp"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

using namespace m54;
static Observation frameScene() {
    Observation s;
    s.at = 100; s.cpu = .7; s.gpu = .85; s.temp = 42; s.batteryTemp = 33;
    s.cpuPsi = .1; s.energy = .55; s.p95 = 25; s.jank = .2;
    s.target = 60; s.frames = 300; s.battery = 75;
    s.awake = s.thermalValid = s.framesValid = s.loadValid = true;
    s.app = "org.test.game";
    return s;
}
static void file(const std::string& path, const std::string& value) { std::ofstream(path) << value; }
static bool has(const std::vector<Choice>& options, const Action& a) {
    for (const auto& c : options) if (c.action == a) return true;
    return false;
}
int main() {
    auto s = frameScene(); Constraints c; c.tier = Tier::Game;
    Action a; a.level = {2, 3, 1, 1};
    assert(Action::fromId(a.id()) == a);
    assert(a.effort() == 7 && Action{}.effort() == 0);
    auto hot = s; hot.temp = 80; assert(safe(a, hot, c) == Action{});
    hot = s; hot.batteryTemp = 44; assert(safe(a, hot, c) == Action{});
    hot = s; hot.thermalValid = false; assert(safe(a, hot, c) == Action{});
    hot = s; hot.framesValid = false; assert(safe(a, hot, c) == Action{});
    hot = s; hot.battery = 8; assert(safe(a, hot, c) == Action{});
    c.allowed[0] = c.allowed[1] = false;
    for (auto x : candidates({}, s, c)) assert(x.action.level[0] == 0 && x.action.level[1] == 0);
    c.allowed = {true, true, true, true};

    // ---- paging observer -------------------------------------------------------------------
    {
        // The whole contract of this channel is that it is measured and nothing else. If any of
        // these four ever fails, an unvalidated signal has started steering the device.
        auto thrashing = s;
        thrashing.pagingValid = true;
        // The p99 of each channel as actually measured on this device, so the scales stay
        // anchored to the report that set them rather than to a number someone liked.
        thrashing.majorFaults = 16484; thrashing.swapIn = 7122; thrashing.fileRefault = 44617;
        const auto quiet = s; // same window, paging simply not measured
        assert(costs(thrashing, c, a) == costs(quiet, c, a));
        assert(reward(thrashing, c, a) == reward(quiet, c, a));
        assert(deficit(thrashing).value == deficit(quiet).value);
        assert(demanding(thrashing) == demanding(quiet) && measurable(thrashing) == measurable(quiet));
        assert(safe(a, thrashing, c) == safe(a, quiet, c));
        // It does reach the critic, which is the point: learning in an unvalidated regime is free.
        const auto loud = features(thrashing, c, a), still = features(quiet, c, a);
        assert(loud[25] > .85 && loud[25] < 1 && loud[26] > .95 && loud[26] < 1);
        // The measured maxima saturate, which is what a scale set at p99 is supposed to do.
        auto storm = thrashing; storm.swapIn = 31507; storm.fileRefault = 56953;
        assert(features(storm, c, a)[25] == 1 && features(storm, c, a)[26] == 1);
        assert(still[25] == 0 && still[26] == 0);
        for (size_t k = 0; k < PagingFreeFeatures; ++k) assert(loud[k] == still[k]);
        // An unmeasured window reads as zero rather than as "no paging happened at a huge rate".
        auto unmeasured = thrashing; unmeasured.pagingValid = false;
        assert(features(unmeasured, c, a)[25] == 0 && features(unmeasured, c, a)[26] == 0);
    }

    // ---- thermal-energy allowance ---------------------------------------------------------
    {
        Constraints capped = c; capped.ceiling = 1;
        Action big; big.level = {4, 4, 4, 1};
        auto limited = safe(big, s, capped);
        for (int i = 0; i < Axes; ++i) assert(limited.level[i] <= 1);
        Budget allowance;
        assert(allowance.ceiling() == 4);
        auto warm = s; warm.temp = 74;
        for (int i = 0; i < 100; ++i) allowance.update(big, warm, c, 6);
        assert(allowance.remaining() < .2 && allowance.ceiling() <= 1);
        auto cool = s; cool.temp = 40;
        for (int i = 0; i < 300; ++i) allowance.update({}, cool, c, 6);
        assert(allowance.remaining() > .9 && allowance.ceiling() == 4);
        // Effort on a cool device must never draw the allowance down on its own.
        Budget freeRide;
        for (int i = 0; i < 200; ++i) freeRide.update(big, cool, c, 6);
        assert(freeRide.remaining() > .99);
    }

    // ---- reward, minimal intervention and features -----------------------------------------
    auto good = s; good.p95 = 16.7; good.jank = .01;
    assert(reward(good, c) > reward(s, c));
    auto expensive = good; expensive.energy = .95;
    double gamePenalty = reward(good, c) - reward(expensive, c);
    c.tier = Tier::Powersave;
    assert(reward(good, c) - reward(expensive, c) > gamePenalty * 3);
    c.tier = Tier::Game;
    Action busy; busy.level = {4, 4, 4, 1};
    assert(reward(good, c, Action{}) > reward(good, c, busy));
    assert(reward(good, c, Action{}) == reward(good, c));
    {
        auto f = features(s, c, busy);
        assert(f[0] == 1 && f[14] == 1 && f[17] == .25);
        for (double x : f) assert(x >= 0 && x <= 1);
        // A saturated thread must reach the critic even when the aggregate looks idle.
        auto pinned = s; pinned.cpu = .31; pinned.cpuPeak = .52; pinned.threadPeak = 1;
        auto g = features(pinned, c, Action{});
        assert(g[7] < .4 && g[18] > .5 && g[19] == 1);
    }

    // ---- deficit channels: agnostic to what produced the demand ----------------------------
    {
        auto d = deficit(s);
        assert(d.valid[FrameChannel] && d.valid[StallChannel] && !d.valid[QueueChannel]);
        assert(d.primary() == d.value[FrameChannel] && d.value[FrameChannel] > 0);

        // No renderer: the frame channel goes away, the stall channel does not, and the deficit
        // falls back to it instead of reporting a perfectly served workload.
        auto headless = s;
        headless.framesValid = false; headless.frames = 0; headless.p95 = 0; headless.jank = 0;
        headless.cpuPsi = .3; headless.memPsi = .2; headless.ioPsi = .1;
        auto h = deficit(headless);
        assert(!h.valid[FrameChannel] && h.valid[StallChannel]);
        assert(h.primary() > .5 && h.primary() == h.value[StallChannel]);

        // The point of the whole change: a stalled frameless window must not score as well as a
        // quiet one. Under the old objective both read as "no lateness", so the best answer in
        // any frameless scenario was always to do nothing.
        auto calm = headless; calm.cpuPsi = .01; calm.memPsi = 0; calm.ioPsi = 0;
        assert(reward(calm, c) > reward(headless, c) + .1);

        // Absence and perfection must not arrive at the critic as the same number.
        auto perfect = s; perfect.p95 = 1000. / s.target; perfect.jank = 0; perfect.slowFrames50 = 0;
        auto fp = features(perfect, c, Action{});
        auto fh = features(headless, c, Action{});
        assert(fp[1] < 1e-9 && fh[1] == 0);    // identical deficit reading ...
        assert(fp[20] == 0 && fh[20] == 1);    // ... told apart only by the absence flag
        assert(fh[22] > .5 && fp[22] < .3);    // and the stall channel carries the frameless one
        for (double x : fp) assert(x >= 0 && x <= 1);
        for (double x : fh) assert(x >= 0 && x <= 1);

        // The runqueue probe reaches the features and stays out of the objective, which is the
        // standing rule for a signal that has not yet been shown to predict stutter here.
        auto probed = s; probed.queueValid = true; probed.queueMs = 6; probed.queueLate = .1;
        auto fq = features(probed, c, Action{});
        assert(fq[21] > .5 && features(s, c, Action{})[21] == 0);
        assert(reward(probed, c) == reward(s, c));
    }

    // ---- relative moves --------------------------------------------------------------------
    {
        auto options = candidates({}, s, c);
        assert(options.size() > 1 && options[0].move == Stay && options[0].action == Action{});
        Action up; up.level[0] = 1;
        assert(has(options, up));
        assert(moveIndex({}, up, s, c) > 0);
        assert(moveIndex({}, busy, s, c) < 0); // not reachable in one step
        auto fromUp = candidates(up, s, c);
        assert(has(fromUp, Action{}));
        assert(moveIndex(up, {}, s, c) > 0);
    }

    Brain brain;
    Planner planner(7);
    auto key = context(s, c, "test");
    assert(key.fine != key.coarse && key.fine.find(key.coarse) == 0);
    auto decision = planner.search(brain, key, s, {}, c, false, 256, 4, 20000);
    assert(decision.simulations > 20 && decision.simulations <= 256 && decision.depth >= 2);
    assert(decision.nodes <= 257 && brain.model.samples == 0 && brain.model.cells.empty());
    assert(brain.windows == 0 && brain.critic.updates == 0);
    auto stress = s; stress.p95 = 40; stress.jank = .45; stress.cpu = .9; stress.gpu = .98;
    Planner stressedPlanner(7);
    // Janking badly with thermal and charge headroom to spare: sitting still is not an answer.
    auto boostDecision = stressedPlanner.search(brain, key, stress, {}, c, true, 256, 4, 20000);
    assert(boostDecision.action != Action{});
    // The same stress inside the thermal guard band may not be answered with the same boost.
    auto guarded = stress; guarded.temp = 74;
    auto guardedDecision = stressedPlanner.search(brain, key, guarded, {}, c, true, 256, 4, 20000);
    for (int i = 0; i < Axes; ++i) assert(guardedDecision.action.level[i] <= 1);
    auto overLimit = stress; overLimit.temp = 76;
    assert(stressedPlanner.search(brain, key, overLimit, {}, c, true, 256, 4, 20000).action == Action{});

    // ---- residual attribution ---------------------------------------------------------------
    auto after = s; after.at += 6;
    auto invalid = after; invalid.app = "org.other.app";
    assert(!brain.model.observe(key, s, {}, a, invalid));
    invalid = after; invalid.frames = 0; assert(!brain.model.observe(key, s, {}, a, invalid));
    invalid = after; invalid.charging = true; assert(!brain.model.observe(key, s, {}, a, invalid));
    invalid = after; invalid.cpu = .1; assert(!brain.model.observe(key, s, {}, a, invalid));
    invalid = after; invalid.p95 = NAN; assert(!brain.model.observe(key, s, {}, a, invalid));
    // A value backup tolerates a workload shift that would poison a per-edge residual.
    invalid = after; invalid.cpu = .25;
    assert(attributable(s, invalid) && !learnable(s, invalid));
    // It also survives the user leaving the app, which a per-app credit cannot.
    invalid = after; invalid.app = "org.other.app";
    assert(backupable(s, invalid) && !attributable(s, invalid) && !learnable(s, invalid));
    // A frameless window IS a backup now: presented frames are one estimator of unserved
    // demand, not the definition of it. What it must never become is a per-edge residual,
    // because without frames there is no evidence to blame the edge with.
    invalid = after; invalid.framesValid = false; invalid.frames = 0; invalid.p95 = 0;
    assert(backupable(s, invalid) && attributable(s, invalid) && !learnable(s, invalid));
    // Deep idle stays out. Nothing was asked of the device, so nothing was answered, and at
    // one window every six seconds it would otherwise become most of what the critic sees.
    { auto idle = invalid;
      idle.cpu = idle.cpuPeak = idle.threadPeak = idle.gpu = 0;
      idle.cpuPsi = idle.memPsi = idle.ioPsi = 0; idle.queueValid = false;
      assert(!demanding(idle) && !measurable(idle) && !backupable(s, idle)); }
    // A screen-off window with real work in it is still a window worth learning from.
    { auto dark = invalid; dark.awake = false;
      assert(demanding(dark) && backupable(s, dark) && !learnable(s, dark));
      assert(features(dark, c, Action{})[23] == 1 && features(s, c, Action{})[23] == 0); }
    invalid = after; invalid.at = s.at + 45;
    assert(!backupable(s, invalid)); // and neither is a pair separated by a long gap
    // Observed regressions must revise the transition model, not just increment a counter.
    Action gpu; gpu.level[1] = 1;
    double old = brain.model.predict(key, s, {}, gpu).p95;
    after.p95 = 60; after.jank = .75;
    for (int i = 0; i < 80; ++i) assert(brain.model.observe(key, s, {}, gpu, after));
    assert(brain.model.predict(key, s, {}, gpu).p95 > old + 25);
    assert(brain.model.count(key, {}, gpu) > 0);

    // Surprising outcomes reopen only the affected workload. No evidence is deleted, and a
    // surprise counts as precisely one observed transition, never as fabricated exploration.
    {
        Model m;
        auto seen = frameScene(), next = seen; next.at += 6;
        auto k = context(seen, c, "drift");
        auto other = context(seen, c, "unrelated");
        Action up; up.level[1] = 1;
        for (int i = 0; i < 12; ++i) {
            assert(m.observe(k, seen, {}, up, next));
            assert(m.observe(k, seen, {}, {}, next));
            assert(m.observe(other, seen, {}, up, next));
        }
        const auto countBefore = m.cells.size(), samplesBefore = m.samples;
        const auto unrelatedId = other.coarse + "/0/" + std::to_string(up.id());
        const auto unrelatedCount = m.cells.at(unrelatedId).count;
        const auto unrelatedMean = m.cells.at(unrelatedId).mean;
        auto shock = next; shock.p95 += 12;
        assert(m.observe(k, seen, {}, {}, shock));
        assert(m.surprises == 1 && m.samples == samplesBefore + 1 && m.cells.size() == countBefore);
        assert(m.count(k, {}, up) <= 1);
        assert(m.cells.at(unrelatedId).count == unrelatedCount && m.cells.at(unrelatedId).mean == unrelatedMean);
        // Without a visible shock at zero effort, alternatives must still become testable
        // again. Age is measured in real accepted samples, not simulated search nodes.
        for (int i = 0; i < 512; ++i) assert(m.observe(k, seen, {}, {}, next));
        assert(m.count(other, {}, up) < 3);
        assert(m.cells.at(unrelatedId).count == unrelatedCount); // retained raw evidence
        assert(m.cells.at(unrelatedId).mean == unrelatedMean);
    }

    // ---- coarse backoff: a cold fine cell inherits the same app's coarse experience ----------
    {
        auto shifted = s; shifted.temp = 58; shifted.batteryTemp = 36;
        auto shiftedKey = context(shifted, c, "test");
        assert(shiftedKey.fine != key.fine && shiftedKey.coarse == key.coarse);
        assert(brain.model.count(shiftedKey, {}, gpu) == 0);
        Brain cold;
        double naive = cold.model.predict(shiftedKey, shifted, {}, gpu).p95;
        double informed = brain.model.predict(shiftedKey, shifted, {}, gpu).p95;
        assert(informed > naive + 10);
    }

    // ---- acceptance gate: asymmetric, evidence-first ------------------------------------------
    {
        Brain gate;
        auto gateKey = context(s, c, "gate");
        Action up; up.level[0] = 1;
        assert(!accept(gate.model, gateKey, s, {}, Action{}, c, true)); // standing still is not a change
        // The cold start admits it does not know what a floor buys, so spending more is not
        // justified by the model alone. It is reached only as bounded novelty, while safe.
        assert(!accept(gate.model, gateKey, s, {}, up, c, false));
        assert(accept(gate.model, gateKey, s, {}, up, c, true));
        auto nothingGained = s; nothingGained.at += 6; // same p95: the boost bought nothing
        for (int i = 0; i < 6; ++i) assert(gate.model.observe(gateKey, s, {}, up, nothingGained));
        assert(!accept(gate.model, gateKey, s, {}, up, c, true)); // novelty budget is spent
        // Releasing keeps a wider budget than spending: it is the recoverable direction.
        assert(accept(gate.model, gateKey, s, up, {}, c, true));
        for (int i = 0; i < 6; ++i) assert(gate.model.observe(gateKey, s, up, {}, nothingGained));
        assert(accept(gate.model, gateKey, s, up, {}, c, true));
    }

    // ---- ambition: the history must separate "learned it is worthless" from "never tried" -----
    {
        Brain gate;
        auto key = context(s, c, "ambition");
        Constraints one = c;                       // one axis open, so the best costlier move is
        one.allowed = {true, false, false, false}; // unambiguous and `tried` is assertable
        Action up; up.level[0] = 1;

        // Cold start. A costlier move exists and has a real margin to clear, nothing has ever
        // been measured behind it, and the model alone cannot justify it -- only novelty can.
        auto cold = ambition(gate.model, key, s, {}, one, false);
        assert(cold.move >= 0 && cold.toll > 0);
        assert(cold.tried == 0 && !cold.accepted);
        assert(ambition(gate.model, key, s, {}, one, true).accepted); // novelty, while exploring

        // Now teach it that the boost genuinely pays. The model, not the novelty budget, must
        // carry it: accepted stays true with exploration OFF, which is the state this device
        // spends a third of its gameplay in once the battery passes 39 C.
        auto better = s; better.at += 6; better.p95 = 12; better.jank = .05;
        for (int i = 0; i < 6; ++i) assert(gate.model.observe(key, s, {}, up, better));
        auto earned = ambition(gate.model, key, s, {}, one, false);
        assert(earned.tried >= 6);
        assert(earned.advantage >= earned.toll && earned.accepted);

        // And the opposite lesson must read differently in the file. Same number of measured
        // windows, no gain in any of them: refused, but with `tried` high -- which is exactly the
        // distinction the column exists to make against a cold `tried` of zero.
        Brain flat;
        auto flatKey = context(s, c, "ambition-flat");
        auto nothing = s; nothing.at += 6;
        for (int i = 0; i < 6; ++i) assert(flat.model.observe(flatKey, s, {}, up, nothing));
        auto refused = ambition(flat.model, flatKey, s, {}, one, false);
        assert(refused.tried >= 6 && !refused.accepted);
        assert(refused.advantage < refused.toll);
    }

    // ---- curiosity reaches the search, not just the gate ----------------------------------------
    {
        auto coldKey = context(s, c, "curious");
        int curious = 0, calm = 0;
        for (uint32_t seed = 1; seed <= 12; ++seed) {
            Brain cold;
            if (Planner(seed).search(cold, coldKey, s, {}, c, true).action != Action{}) ++curious;
            if (Planner(seed).search(cold, coldKey, s, {}, c, false).action != Action{}) ++calm;
        }
        // A cold model must be willing to look; without headroom it must not.
        assert(curious > 0 && curious >= calm);
    }

    after = s; after.at += 6;
    for (int i = 0; i < 30; ++i) assert(brain.model.observe(key, s, {}, {}, after));
    auto learned = planner.search(brain, key, s, {}, c, false, 256, 4, 20000);
    assert(learned.action != gpu);
    auto total = brain.model.samples;
    planner.search(brain, key, s, {}, c, false);
    assert(brain.model.samples == total); // simulations never manufacture experience

    // ---- critic: TD(0) separates a good window from a bad one --------------------------------
    {
        auto goodFeatures = features(good, c, Action{});
        auto badFeatures = features(s, c, Action{});
        auto goodCosts = costs(good, c, Action{}), badCosts = costs(s, c, Action{});
        for (int i = 0; i < 1500; ++i) {
            brain.critic.learn(goodFeatures, goodCosts, goodFeatures, c.tier);
            brain.critic.learn(badFeatures, badCosts, badFeatures, c.tier);
        }
        assert(brain.critic.value(goodFeatures, c.tier) > brain.critic.value(badFeatures, c.tier));
        assert(brain.critic.value(goodFeatures, c.tier) > 1);
        assert(brain.critic.trust() > .9);
        // Windows measured under Game teach Economia too: the terms are shared, only the price
        // list differs. This is the whole point of learning the costs instead of one profile's
        // return -- the old design left Economia at zero updates on a device that had measured
        // five hundred windows.
        assert(brain.critic.value(goodFeatures, Tier::Powersave) >
               brain.critic.value(badFeatures, Tier::Powersave));
        // And they remain different objectives: Economia prices energy above smoothness.
        {
            auto thirsty = good; thirsty.energy = 1.2;
            auto smoothLess = good; smoothLess.p95 = 26; smoothLess.jank = .3;
            assert(reward(thirsty, {Tier::Powersave}) < reward(smoothLess, {Tier::Powersave}));
            assert(reward(thirsty, {Tier::Game}) > reward(smoothLess, {Tier::Game}));
        }
        for (const auto& bank : brain.critic.weights)
            for (double w : bank) assert(std::isfinite(w) && std::abs(w) <= 8);
        // A single window cannot rewrite the value function.
        auto snapshot = brain.critic.weights[0];
        brain.critic.learn(goodFeatures, badCosts, badFeatures, c.tier);
        double drift = 0;
        for (size_t i = 0; i < Critic::Dim; ++i) drift = std::max(drift, std::abs(snapshot[i] - brain.critic.weights[0][i]));
        assert(drift < .05);
    }

    // ---- the same critic, a scenario with no game in it --------------------------------------
    {
        // The point of the exercise. Nothing here renders: no frames, no cadence, no jank, and
        // under the old contract not one of these windows could ever have reached the critic.
        // A planner that situates itself in whatever scenario it is in has to be able to rank
        // them, and ranking them means separating "stalled" from "running clean" using the
        // evidence a frameless workload actually leaves behind.
        auto headless = s;
        headless.framesValid = false; headless.frames = 0; headless.p95 = 0; headless.jank = 0;
        headless.slowFrames50 = 0;
        auto stalled = headless; stalled.cpuPsi = .35; stalled.memPsi = .25; stalled.ioPsi = .15;
        auto clean = headless; clean.cpuPsi = .02; clean.memPsi = 0; clean.ioPsi = .01;
        assert(measurable(stalled) && measurable(clean));
        assert(reward(clean, c, Action{}) > reward(stalled, c, Action{}));

        Brain fresh;
        auto stalledFeatures = features(stalled, c, Action{});
        auto cleanFeatures = features(clean, c, Action{});
        for (int i = 0; i < 1500; ++i) {
            fresh.critic.learn(cleanFeatures, costs(clean, c, Action{}), cleanFeatures, c.tier);
            fresh.critic.learn(stalledFeatures, costs(stalled, c, Action{}), stalledFeatures, c.tier);
        }
        assert(fresh.critic.value(cleanFeatures, c.tier) > fresh.critic.value(stalledFeatures, c.tier) + .5);

        // And learning that scenario must not cost it the one it already knew. The regime flag is
        // what lets one linear value function hold both without either overwriting the other.
        auto renderGood = features(good, c, Action{}), renderBad = features(s, c, Action{});
        for (int i = 0; i < 1500; ++i) {
            fresh.critic.learn(renderGood, costs(good, c, Action{}), renderGood, c.tier);
            fresh.critic.learn(renderBad, costs(s, c, Action{}), renderBad, c.tier);
            fresh.critic.learn(cleanFeatures, costs(clean, c, Action{}), cleanFeatures, c.tier);
            fresh.critic.learn(stalledFeatures, costs(stalled, c, Action{}), stalledFeatures, c.tier);
        }
        assert(fresh.critic.value(renderGood, c.tier) > fresh.critic.value(renderBad, c.tier));
        assert(fresh.critic.value(cleanFeatures, c.tier) > fresh.critic.value(stalledFeatures, c.tier));
        for (double w : fresh.critic.weights[0]) assert(std::isfinite(w) && std::abs(w) <= 8);
    }

    // ---- policy prior ------------------------------------------------------------------------
    {
        auto options = candidates({}, s, c);
        auto flat = brain.prior.distribution(key.coarse, c.tier, options);
        double sum = 0;
        for (double x : flat) { assert(x > 0); sum += x; }
        assert(std::abs(sum - 1) < 1e-9);
        for (size_t i = 1; i < flat.size(); ++i) assert(std::abs(flat[i] - flat[0]) < 1e-9);
        const int move = options[1].move;
        for (int i = 0; i < 60; ++i) brain.prior.reinforce(key.coarse, c.tier, move, options, 1);
        auto shaped = brain.prior.distribution(key.coarse, c.tier, options);
        assert(shaped[1] > flat[1] * 1.2);
        double floor = Prior::Floor / static_cast<double>(options.size());
        for (double x : shaped) assert(x >= floor * .999); // exploration never collapses
        for (int i = 0; i < 400; ++i) brain.prior.reinforce(key.coarse, c.tier, move, options, 1);
        for (auto x : brain.prior.tier[0]) assert(std::abs(x) <= 1.5f);
        brain.prior.reinforce(key.coarse, c.tier, 99, options, 1); // unknown move is ignored
        brain.prior.reinforce(key.coarse, c.tier, move, options, NAN);
        for (double x : brain.prior.distribution(key.coarse, c.tier, options)) assert(std::isfinite(x));
    }

    // ---- replay: idle rehearsal uses measured windows only -------------------------------------
    {
        Brain rehearsal;
        Planner shuffler(3);
        auto goodFeatures = features(good, c, Action{});
        auto badFeatures = features(s, c, Action{});
        for (int i = 0; i < 600; ++i) {
            rehearsal.replay.add(goodFeatures, costs(good, c, Action{}), goodFeatures, c.tier, Discount, shuffler.random());
            rehearsal.replay.add(badFeatures, costs(s, c, Action{}), badFeatures, c.tier, Discount, shuffler.random());
        }
        assert(rehearsal.replay.samples.size() == Replay::Capacity && rehearsal.replay.seen == 1200);
        assert(rehearsal.replay.rehearse(rehearsal.critic, shuffler.random(), 64) == 64);
        assert(rehearsal.critic.updates == 0); // rehearsal is not new evidence
        for (int i = 0; i < 400; ++i) rehearsal.replay.rehearse(rehearsal.critic, shuffler.random(), 64);
        assert(rehearsal.critic.value(goodFeatures, c.tier) > rehearsal.critic.value(badFeatures, c.tier));
        for (double w : rehearsal.critic.weights[0]) assert(std::isfinite(w) && std::abs(w) <= 8);
        Brain empty;
        assert(empty.replay.rehearse(empty.critic, shuffler.random(), 64) == 0);
    }

    // ---- persistence ---------------------------------------------------------------------------
    brain.windows = 41;
    brain.budget = .625;
    brain.replay.add(features(good, c, Action{}), costs(good, c, Action{}), features(s, c, Action{}), c.tier, Discount, planner.random());
    auto data = brain.serialize("device1");
    Brain restored;
    assert(restored.deserialize(data, "device1"));
    assert(restored.model.samples == total && restored.windows == 41);
    assert(restored.model.cells.size() == brain.model.cells.size());
    assert(restored.prior.contexts.size() == brain.prior.contexts.size());
    assert(restored.replay.samples.size() == brain.replay.samples.size());
    assert(std::abs(restored.budget - .625) < 1e-6);
    assert(restored.critic.updates == brain.critic.updates);
    {
        auto probe = features(good, c, Action{});
        assert(std::abs(restored.critic.value(probe, c.tier) - brain.critic.value(probe, c.tier)) < 1e-6);
        auto options = candidates({}, s, c);
        auto before = brain.prior.distribution(key.coarse, c.tier, options);
        auto now = restored.prior.distribution(key.coarse, c.tier, options);
        for (size_t i = 0; i < before.size(); ++i) assert(std::abs(before[i] - now[i]) < 1e-6);
    }
    assert(!restored.deserialize(data, "device2"));
    auto corrupt = data; corrupt[30] ^= 1;
    assert(!restored.deserialize(corrupt, "device1"));
    assert(restored.model.samples == total); // invalid input does not partially replace live memory
    assert(!restored.deserialize("M54_BRAIN_3 device1 1 1 1 1\nCHECK 0\n", "device1"));
    assert(!restored.deserialize("", "device1"));
    {
        // A file that survives the checksum but claims impossible values is still rejected.
        std::string body = "M54_BRAIN_5 device1 0 0 0 0\nV 0";
        for (size_t i = 0; i < 2 * Critic::Dim; ++i) body += " 900";
        body += '\n';
        assert(!restored.deserialize(body + "CHECK " + std::to_string(hash(body)) + '\n', "device1"));
    }
    {
        // ---- a brain saved before the paging channels ----------------------------------------
        // The width changed; the meaning of every weight below it did not. So the file loads,
        // the value function it encodes is reproduced exactly on the features it knew about, the
        // two new ones start at zero, and the replay buffer survives -- which is the difference
        // between a schema change and throwing away four thousand measured windows.
        std::string narrow = "M54_BRAIN_5 device1 12 12 0 0\n";
        auto row = [](size_t width, double weight) {
            std::string out;
            for (size_t k = 0; k < width; ++k) out += ' ' + std::to_string(weight);
            for (size_t k = 0; k < width; ++k) out += " 0";
            return out;
        };
        for (size_t term = 0; term < static_cast<size_t>(Costs); ++term)
            narrow += "V " + std::to_string(term) + row(PagingFreeFeatures, .25) + '\n';
        // Rewritten as the version that predates the channels, byte for byte apart from the tag.
        narrow.replace(0, 11, "M54_BRAIN_4");
        Brain aged;
        assert(aged.deserialize(narrow + "CHECK " + std::to_string(hash(narrow)) + '\n', "device1"));
        for (size_t term = 0; term < static_cast<size_t>(Costs); ++term) {
            for (size_t k = 0; k < PagingFreeFeatures; ++k)
                assert(std::abs(aged.critic.weights[term][k] - .25) < 1e-9);
            for (size_t k = PagingFreeFeatures; k < Critic::Dim; ++k)
                assert(aged.critic.weights[term][k] == 0 && aged.critic.scale[term][k] == 0);
        }
        assert(aged.windows == 12 && aged.model.samples == 12);
        // Read at the current width the same bytes would swallow the next row and be rejected,
        // which is exactly the failure the version tag exists to prevent.
        std::string mislabelled = narrow;
        mislabelled.replace(0, 11, "M54_BRAIN_5");
        Brain confused;
        assert(!confused.deserialize(mislabelled + "CHECK " + std::to_string(hash(mislabelled)) + '\n', "device1"));
    }
    {
        // ---- migration from the per-profile brain --------------------------------------------
        // Two things have to survive a build that stopped keying physics by preference: the value
        // functions that were actually measured, and the residual cells that were split by a field
        // with no physical meaning. Both are checked here against a hand-built version 3 file.
        const std::string oldCfg = "111", otherCfg = "222", newCfg = "999";
        auto legacyBody = [&](const std::array<std::array<double, Critic::Dim>, 3>& weights,
                              const std::array<uint32_t, 3>& updates) {
            std::ostringstream out;
            out << "M54_BRAIN_3 device1 40 40 7 0\n" << std::setprecision(9);
            out << "B 1\n";
            // The same app, target, charge, power and axis mask, and the same edge: one device,
            // one piece of physics, measured once under Game and once under Balanceado.
            const std::string body = "7:60:0:1:15:";
            out << "C 0:" << body << oldCfg << "/0/5 4 10  20 .1 .2 .3 .4  1 0 0 0 0\n";
            out << "C 1:" << body << oldCfg << "/0/5 4 11  10 .1 .2 .3 .4  1 0 0 0 0\n";
            // And one cell from a configuration this build cannot reconstruct.
            out << "C 1:" << body << otherCfg << "/0/5 4 12  99 .1 .2 .3 .4  1 0 0 0 0\n";
            for (size_t t = 0; t < 3; ++t) {
                out << "V " << t << ' ' << updates[t];
                for (size_t k = 0; k < PagingFreeFeatures; ++k) out << ' ' << weights[t][k];
                for (size_t k = 0; k < PagingFreeFeatures; ++k) out << " 0";
                out << '\n';
                out << "P " << t;
                for (int m = 0; m < Moves; ++m) out << " 0";
                out << '\n';
            }
            out << "Q 0:" << body << oldCfg << " 5";
            for (int m = 0; m < Moves; ++m) out << " .5";
            out << '\n';
            out << "Q 1:" << body << otherCfg << " 6";
            for (int m = 0; m < Moves; ++m) out << " .5";
            out << '\n';
            return out.str();
        };
        std::array<std::array<double, Critic::Dim>, 3> weights{};
        std::array<uint32_t, 3> updates{{300, 900, 0}};
        std::mt19937 noise(19);
        std::uniform_real_distribution<double> spread(-.2, .2);
        // A brain of that vintage carried PagingFreeFeatures weights; the ones added since read
        // zero out of it, which is what the value check below relies on.
        for (size_t t = 0; t < 2; ++t)
            for (size_t k = 0; k < PagingFreeFeatures; ++k) weights[t][k] = spread(noise);
        const auto body = legacyBody(weights, updates);
        const std::map<std::string, std::string> rehome{{oldCfg, newCfg}};

        Brain moved;
        assert(moved.deserialize(body + "CHECK " + std::to_string(hash(body)) + '\n', "device1", rehome));

        // The two profiles' halves of the same edge are one cell again, and their evidence is
        // pooled rather than one of them winning: the residual sits between 20 and 10, and the
        // count is the sum.
        const std::string edge = "7:60:0:1:15:" + newCfg + "/0/5";
        assert(moved.model.cells.count(edge) == 1);
        assert(moved.model.cells.at(edge).count == 8);
        assert(std::abs(moved.model.cells.at(edge).mean[0] - 15) < 1e-9);
        assert(moved.model.cells.at(edge).variance[0] > 20); // the disagreement is kept, not hidden
        // A cell from an unreconstructable configuration is dropped, not relabelled.
        assert(moved.model.cells.size() == 1);
        // A policy cell keeps its profile and only moves configuration.
        assert(moved.prior.contexts.count("0:7:60:0:1:15:" + newCfg) == 1);
        assert(moved.prior.contexts.size() == 1);

        // The warm start reproduces both measured value functions exactly, for any state, which
        // is what makes it a change of representation rather than a loss of 1200 windows.
        std::uniform_real_distribution<double> unit(0, 1);
        for (int trial = 0; trial < 20; ++trial) {
            Features probe{};
            for (auto& x : probe) x = unit(noise);
            probe[0] = 1; // features() always carries the bias, and the identity is stated on it
            for (size_t t = 0; t < 2; ++t) {
                double legacyValue = 0;
                for (size_t k = 0; k < Critic::Dim; ++k) legacyValue += weights[t][k] * probe[k];
                const double bounded = std::max(-ValueLimit, std::min(ValueLimit, legacyValue));
                assert(std::abs(moved.critic.value(probe, static_cast<Tier>(t)) - bounded) < 1e-6);
            }
        }
        // Economia was never selected while anything was measured, so it constrained nothing. It
        // still gets a value function, because the terms are shared -- which is the entire reason
        // for the change, and is exactly what the old design could not do.
        Features probe{};
        for (auto& x : probe) x = unit(noise);
        probe[0] = 1;
        assert(std::isfinite(moved.critic.value(probe, Tier::Powersave)));
        // Confidence starts over: the seeded weights reproduce what was measured, but the
        // directions no profile constrained are a guess, and a guess is not evidence.
        assert(moved.critic.updates == 0 && moved.critic.trust() == 0);
        // Stored replay windows from the old format are dropped rather than invented.
        assert(moved.replay.samples.empty() && moved.replay.seen == 0);
        // Without a rehome map there is nothing to migrate onto, so the old cells are dropped
        // instead of being kept under identities this build would never look up.
        Brain blind;
        assert(blind.deserialize(body + "CHECK " + std::to_string(hash(body)) + '\n', "device1"));
        assert(blind.model.cells.empty() && blind.prior.contexts.empty());
    }
    // The profile changes the policy key and nothing else: physics is not preference.
    {
        Constraints economy = c; economy.tier = Tier::Powersave;
        assert(context(s, c, "cfg").coarse == context(s, economy, "cfg").coarse);
        assert(context(s, c, "cfg").fine == context(s, economy, "cfg").fine);
        assert(context(s, c, "cfg").policy != context(s, economy, "cfg").policy);
        // A different set of permitted axes IS a different tuning surface.
        Constraints narrowed = c; narrowed.allowed[1] = false;
        assert(context(s, c, "cfg").coarse != context(s, narrowed, "cfg").coarse);
        assert(context(s, c, "cfg").coarse != context(s, c, "other").coarse);
    }

    // ---- telemetry parsing ----------------------------------------------------------------------
    assert(foreground("topResumedActivity=ActivityRecord{12 u0 org.test.game/.Activity t2}") == "org.test.game");
    assert(foreground("mResumedActivity: ActivityRecord{12 u0 org.test.game/.Activity t2}") == "org.test.game");
    assert(foreground("topResumedActivity=null").empty());
    assert(chooseLayer("RequestedLayerState{org.test.game/Activity$_55#92 parentId=20}\n", "org.test.game") == "org.test.game/Activity$_55#92");
    FrameTracker tracker;
    // The header is the panel's vsync period. 8333333 ns is 120 Hz, which this M54 runs at.
    std::string frames = "8333333\n";
    for (int64_t i = 0; i < 60; ++i) frames += "0 " + std::to_string(99000000000LL + i * 16666667) + " 0\n";
    frames += "0 9223372036854775807 0\n0 0 0\n";
    tracker.addLatency(frames, 100); tracker.finish(after, 120);
    assert(after.frames == 59 && after.framesValid && after.p95 > 16 && after.p95 < 17);
    // Presenting every 16.7 ms on a 120 Hz panel is a 60 fps app, not a late 120 fps one.
    assert(after.target == 60);
    tracker.addLatency(frames, 100); tracker.finish(after, 120);
    assert(after.frames == 0 && !after.framesValid); // repeated SF ring buffer cannot become new experience
    tracker.reset(); tracker.addLatency(frames, 110); tracker.finish(after, 120); assert(after.frames == 0);
    {
        std::string full = "8333333\n";
        for (int64_t i = 0; i < 60; ++i) full += "0 " + std::to_string(99000000000LL + i * 8333333) + " 0\n";
        FrameTracker fast; Observation probe;
        fast.addLatency(full, 100); fast.finish(probe, 120);
        assert(probe.framesValid && probe.target == 120 && probe.p95 < 9);
        // A user ceiling still wins: asked for 60, the engine must not chase 120.
        FrameTracker capped; Observation limited;
        capped.addLatency(full, 100); capped.finish(limited, 60);
        assert(limited.target == 60);
        // Stutter must not be excused by re-reading the app as a slower one. Half the frames
        // land at 8.3 ms and half at 25 ms: the cadence is still 120, and the jank is reported.
        std::string mixed = "8333333\n";
        int64_t at = 99000000000LL;
        for (int i = 0; i < 60; ++i) { at += (i % 2) ? 25000000 : 8333333; mixed += "0 " + std::to_string(at) + " 0\n"; }
        FrameTracker stutter; Observation janky;
        stutter.addLatency(mixed, 100); stutter.finish(janky, 120);
        assert(janky.target == 120 && janky.jank > .3);
        // A steady 30 fps app that emits an occasional back-to-back pair is a 30 fps app. Read
        // from a low percentile those few bursts declare 120 and manufacture 86% jank on a
        // scene where the GPU is idling -- observed on this device in NTE's lobby.
        std::string lobby = "8333333\n";
        at = 99000000000LL;
        for (int i = 0; i < 60; ++i) { at += (i % 12 == 0) ? 8333333 : 33333333; lobby += "0 " + std::to_string(at) + " 0\n"; }
        FrameTracker steady; Observation calm;
        steady.addLatency(lobby, 100); steady.finish(calm, 120);
        assert(calm.target == 30 && calm.jank < .1);
        // The stream that broke the objective on this device: a GPU-bound app presenting about
        // 20 fps in bursts. Exactly 15% of its INTERVALS are 8.3 ms apart, which cleared the old
        // count-based bar and declared a 120 fps intent -- while those bursts occupy 2.5% of the
        // window's time. Same p95, deficit 1.000 instead of 0.131, and the critic trained on both
        // labels for the same device state. Measured against time, the burst is small.
        std::string bursty = "8333333\n";
        at = 97000000000LL;
        double burstTime = 0, wallTime = 0;
        int bursts = 0, intervals = 0;
        for (int i = 0; i < 60; ++i) {
            const int64_t step = (i % 6 == 0) ? 8333333 : 57350000;
            at += step;
            bursty += "0 " + std::to_string(at) + " 0\n";
            if (i == 0) continue;                 // the first timestamp only seeds the cursor
            ++intervals; wallTime += step;
            if (step == 8333333) { ++bursts; burstTime += step; }
        }
        // The two ways of asking "how much of this window ran at 120 Hz" disagree by a factor of
        // six on the same stream, and the old bar sat exactly between them.
        assert(bursts / double(intervals) >= .15);
        assert(burstTime / wallTime < .03);
        FrameTracker gpuBound; Observation strained;
        gpuBound.addLatency(bursty, 100); gpuBound.finish(strained, 120);
        assert(strained.framesValid && strained.target > 17 && strained.target < 24);
        assert(strained.jank < .1);               // it is slow, not late against its own intent
        // Raising the target is evidence-gated, not instant. Adjacent divisors overlap -- the
        // +/-25% windows around 41.7, 33.3 and 25 ms leave no gap -- so on a jittery stream which
        // one clears the bar first is decided by noise, and adopting it immediately turned that
        // noise into target changes in 17 of 160 measured windows. A genuinely faster app is
        // still picked up, three windows later.
        FrameTracker gated; Observation seen;
        int64_t clock = 96000000000LL;
        auto window = [&](FrameTracker& t, Observation& o, int64_t step, int count, double now) {
            std::string dump = "8333333\n";
            for (int i = 0; i < count; ++i) { clock += step; dump += "0 " + std::to_string(clock) + " 0\n"; }
            t.addLatency(dump, now); t.finish(o, 120);
        };
        window(gated, seen, 33333333, 60, 98);
        assert(seen.target == 30 && seen.cadenceSeen == 30);
        for (int settled = 0; settled < 2; ++settled) {
            window(gated, seen, 16666667, 120, 100 + settled * 2);
            // The window itself says 60; the target does not move on its word alone.
            assert(seen.cadenceSeen == 60 && seen.target == 30);
        }
        window(gated, seen, 16666667, 120, 104);
        assert(seen.cadenceSeen == 60 && seen.target == 60);
        // And it is consecutive evidence, not cumulative: one window back at 30 clears the count.
        FrameTracker flapping; Observation flap;
        clock = 96000000000LL;
        window(flapping, flap, 33333333, 60, 98);
        window(flapping, flap, 16666667, 120, 100);
        window(flapping, flap, 33333333, 60, 102);
        window(flapping, flap, 16666667, 120, 104);
        window(flapping, flap, 16666667, 120, 106);
        assert(flap.cadenceSeen == 60 && flap.target == 30);
    }

    // Consecutive windows of a 30 fps stream must count only new presentations. Each poll
    // repeats the compositor's trailing three seconds; controller transitions must not reset
    // this cursor. The old observe path counted ~8 seconds then divided by 6 (about 40 fps).
    {
        FrameTracker tracker;
        auto poll = [&](int second) {
            std::string dump = "8333333\n";
            for (int frame = (second - 3) * 30 + 1; frame <= second * 30; ++frame)
                dump += "0 " + std::to_string(100000000000LL + frame * 1000000000LL / 30) + " 0\n";
            tracker.addLatency(dump, 100 + second);
        };
        poll(3); Observation warm; tracker.finish(warm, 120);
        for (int window = 0; window < 4; ++window) {
            for (int second = 4 + 6 * window; second <= 9 + 6 * window; ++second) poll(second);
            Observation measured; tracker.finish(measured, 120);
            assert(measured.frames == 180 && measured.target == 30);
            assert(std::abs(measured.frameTimeMs - 6000) < .001);
            assert(std::abs(measured.frameEnd - measured.frameStart - 6) < 1e-6);
            assert(measured.slowFrames50 == 0);
        }
        // A real stall is part of the presentation stream, including one longer than 500 ms.
        const auto previous = tracker.last;
        tracker.addLatency("8333333\n0 " + std::to_string(previous + 750000000LL) + " 0\n", 128);
        Observation stalled; tracker.finish(stalled, 120);
        assert(stalled.frames == 1 && stalled.slowFrames50 == 1 && stalled.frameTimeMs == 750);
    }

    // ---- ownership, journal and crash recovery ----------------------------------------------------
    char temp[] = "/tmp/m54-engine-XXXXXX"; auto root = mkdtemp(temp); assert(root);
    std::string dir(root);
    file(dir + "/min", "100"); file(dir + "/max", "1000"); file(dir + "/table", "1000 800 600 400 200 100");
    std::vector<Node> nodes{{dir + "/min", dir + "/max", dir + "/table", 0}};
    {
        file(dir + "/pelt", "1");
        auto withPelt = nodes;
        withPelt.push_back({dir + "/pelt", "", "", 3});
        Actuator writer(dir, withPelt);
        const auto enabled = writer.constrain(c, {}, false);
        assert(enabled.allowed[0] && enabled.allowed[3]);
        Action boost; boost.level[3] = 1;
        assert(writer.apply(boost, enabled) && number(readText(dir + "/pelt")) == 2);
        // A config transition restores ownership before planning on the restricted surface.
        assert(writer.restore());
        const auto disabled = writer.constrain(c, {{"adaptive_pelt", "0"}}, false);
        assert(disabled.allowed[0] && !disabled.allowed[3]);
        for (const auto& choice : candidates(boost, s, disabled)) assert(choice.action.level[3] == 0);
        assert(writer.apply(safe(boost, s, disabled), disabled));
        assert(number(readText(dir + "/pelt")) == 1);
        assert(!writer.constrain(c, {{"adaptive_pelt", "1"}}, true).allowed[3]);
        file(dir + "/pelt", "2");
        Actuator fast(dir, withPelt);
        assert(fast.constrain(c, {}, false).allowed[3]);
        Action boost4; boost4.level[3] = 2;
        assert(fast.apply(boost4, enabled) && number(readText(dir + "/pelt")) == 4);
        assert(fast.restore() && number(readText(dir + "/pelt")) == 2);
        file(dir + "/pelt", "4");
        Actuator maxed(dir, withPelt);
        assert(!maxed.constrain(c, {}, false).allowed[3]);
        assert(maxed.constrain(c, {}, false).allowed[0]);
    }
    {
        // A floor we do not own may be moved under us between windows -- Samsung's top-app QoS
        // boost does exactly this to scaling_min_freq. The baseline has to follow it, or a
        // daemon that started during a boost freezes the boosted value as "factory" and every
        // level below it silently becomes a no-op.
        file(dir + "/min", "600");
        Actuator drifting(dir, nodes);
        file(dir + "/min", "100");                  // the boost let go before we ever wrote
        Action low; low.level[0] = 1;               // asks for a floor above 100, below 600
        assert(drifting.apply(low, c));
        assert(number(readText(dir + "/min")) > 100 && number(readText(dir + "/min")) < 600);
        assert(drifting.restore() && number(readText(dir + "/min")) == 100); // 100, not the stale 600
        file(dir + "/min", "100");
    }
    {
        Actuator writer(dir, nodes); Action boost; boost.level[0] = 4;
        assert(writer.apply(boost, c)); assert(number(readText(dir + "/min")) > 100);
        // Recreate after a simulated daemon crash, recovering the persisted ownership journal.
    }
    {
        Actuator recovery(dir, nodes); assert(recovery.restore()); assert(number(readText(dir + "/min")) == 100);
        Action boost; boost.level[0] = 3; assert(recovery.apply(boost, c));
        file(dir + "/min", "500"); assert(!recovery.verified());
        assert(recovery.restore()); assert(number(readText(dir + "/min")) == 500); // foreign writer retained
    }
    file(dir + "/min", "100");
    file(dir + "/gpu_min", "100");
    {
        auto partial = nodes;
        partial.push_back({dir + "/gpu_min", dir + "/missing_max", dir + "/table", 1});
        Actuator writer(dir, partial); Action boost; boost.level = {3, 2, 0, 0};
        assert(!writer.apply(boost, c)); assert(writer.rejectedAxis() == 1);
        assert(writer.restore()); assert(number(readText(dir + "/min")) == 100);
    }
    {
        // The allowance ceiling reaches the actuator through safe(), not through a separate path.
        Constraints capped = c; capped.ceiling = 0;
        Actuator writer(dir, nodes); Action boost; boost.level[0] = 4;
        assert(safe(boost, s, capped) == Action{});
        assert(writer.apply(safe(boost, s, capped), capped));
        assert(number(readText(dir + "/min")) == 100);
    }
    nodes[0].axis = 1; c.tier = Tier::Powersave;
    {
        Actuator writer(dir, nodes); assert(writer.apply({}, c)); assert(number(readText(dir + "/max")) == 600);
        assert(writer.restore()); assert(number(readText(dir + "/max")) == 1000);
        c.allowed[1] = false; assert(writer.apply({}, c)); assert(number(readText(dir + "/max")) == 1000);
    }
    {
        Observation lowMemory;
        lowMemory.awake = lowMemory.framesValid = true;
        lowMemory.app = "example.app";
        lowMemory.memAvailKb = 500000;
        std::map<std::string, std::string> cfg;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        cfg["game_ram_clear"] = "1";
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        cfg["adaptive_ram_management"] = "0";
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        cfg["adaptive_ram_management"] = "1";
        assert(automaticRamTrimDue(cfg, lowMemory, false, false, 20));
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 19));
        assert(!automaticRamTrimDue(cfg, lowMemory, true, false, 120));
        assert(!automaticRamTrimDue(cfg, lowMemory, false, true, 120));
        for (const char* mode : {"observe", "off", "invalid"}) {
            cfg["adaptive_mode"] = mode;
            assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        }
        cfg["adaptive_mode"] = "active";
        lowMemory.memAvailKb = 1000000;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 59));
        assert(automaticRamTrimDue(cfg, lowMemory, false, false, 60));
        lowMemory.framesValid = false;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        lowMemory.framesValid = true; lowMemory.awake = false;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        lowMemory.awake = true; lowMemory.app.clear();
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        lowMemory.app = "example.app"; lowMemory.memAvailKb = 0;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        lowMemory.memAvailKb = 2000000;
        assert(!automaticRamTrimDue(cfg, lowMemory, false, false, 120));
        lowMemory.memPsi = .09;
        assert(automaticRamTrimDue(cfg, lowMemory, false, false, 120));
    }
    {
        // One armed trim spans exactly the pair ending at the next window, measured or not.
        // This is the state machine behind the daemon's `trim_skipped` credit: the spanned
        // pair must train nothing, and learning resumes on the following window.
        TrimGate g;
        assert(!g.armed() && !g.closeWindow(1500000));
        g.arm(1000000);
        assert(g.armed());
        assert(g.closeWindow(1250000));
        assert(!g.armed() && g.measured && g.freedKb == 250000);
        assert(!g.closeWindow(1300000));
        // Foreground reallocations make the signed delta negative; that is data, not zero.
        g.arm(1000000);
        assert(g.closeWindow(800000) && g.freedKb == -200000);
        // Unreadable window: still spanned, still consumed exactly once, still unmeasured.
        TrimGate u;
        u.arm(1000000);
        assert(u.closeWindow(0) && !u.measured && !u.armed());
        assert(!u.closeWindow(2000000));
        assert(!u.measured);
    }
    {
        // Stable identity versus transient permission: a two-minute QoS hold changes what the
        // search may touch, never the key the model learns under.
        Constraints base;
        RefusalState st;
        const std::string id = "identity-test";
        const auto keyBase = context(s, base, id);
        axisRejected(st, 0, 1000);
        Constraints eff = withTransient(base, st, 1000);
        assert(base.allowed[0] && !eff.allowed[0]);
        const auto keySame = context(s, base, id);
        const auto keyEff = context(s, eff, id);
        assert(keySame.fine == keyBase.fine && keySame.coarse == keyBase.coarse);
        assert(keyEff.fine != keyBase.fine && keyEff.coarse != keyBase.coarse);
        // The named step keys on the base by contract: same fields as context-from-base,
        // never the transient mask — even with an axis latched off. (The remaining surface
        // is the call site in main.cpp using this helper; a return to inline context()
        // with the transient mask would be a visible call-site change, not a silent one.)
        const auto pinned = stableKey(s, base, id);
        assert(pinned.fine == keyBase.fine && pinned.coarse == keyBase.coarse &&
               pinned.policy == keyBase.policy);
        assert(pinned.fine != keyEff.fine && pinned.coarse != keyEff.coarse);
        assert(!axisAvailable(st, 0, 1000) && axisAvailable(st, 0, 1000 + RefusalBackoffSec));
        // Five rapid rejections latch the axis; quiet time earns it back for a re-probe, and a
        // failed probe latches it again instead of flapping every window.
        RefusalState storm;
        for (int i = 0; i < 5; ++i) axisRejected(storm, 2, 2000 + i);
        assert(!axisAvailable(storm, 2, 3000));
        decayRefusals(storm, 3000 + RefusalDecaySec);
        assert(storm.count[2] == 4 && axisAvailable(storm, 2, 3000 + RefusalDecaySec));
        axisRejected(storm, 2, 3000 + RefusalDecaySec);
        assert(storm.count[2] == 5 && !axisAvailable(storm, 2, 3000 + RefusalDecaySec));
        // A stable denial stays denied no matter how quiet it gets.
        Constraints off = base; off.allowed[1] = false;
        assert(!withTransient(off, RefusalState{}, 1e9).allowed[1]);
    }
    {
        // PELT redundancy is recorded, not corrected here: levels 0 and 1 write the same 2x
        // baseline (platform.cpp maps both onto it), yet they price differently. Both the
        // obvious corrections were tried and both breach the furnace — canonicalising 1 onto
        // 0 strands the boost behind a two-step reach it used to cross in one window, and
        // binary effort lets the search wander the free 0<->1 edge until it diffuses into the
        // boost. Either needs the transition model to price PELT heat first (heuristic() has
        // a dp term for energy but none for temperature), which is device measurement, not a
        // constant to tune until the synthetic device passes.
        Action redundant; redundant.level[3] = 1;
        assert(redundant.id() != Action{}.id());
        assert(redundant.effort() == 1);
    }
    {
        // Generations shift instead of overwriting a single `.1`.
        file(dir + "/rot", "new");
        file(dir + "/rot.1", "one");
        file(dir + "/rot.2", "two");
        rotateGenerations(dir + "/rot", 3);
        assert(readText(dir + "/rot", 16).empty());
        assert(readText(dir + "/rot.1", 16) == "new");
        assert(readText(dir + "/rot.2", 16) == "one");
        assert(readText(dir + "/rot.3", 16) == "two");
        file(dir + "/rot", "newer");
        file(dir + "/rot.3", "stale");
        rotateGenerations(dir + "/rot", 3);
        assert(readText(dir + "/rot.3", 16) == "one");
    }
    for (const auto& f : globPaths(dir + "/*")) unlink(f.c_str());
    rmdir(dir.c_str());
    std::cout << "engine: PUCT search, critic, policy prior, replay, allowance, backoff, persistence,\n"
                 "        telemetry, ownership and crash recovery passed\n";
}
