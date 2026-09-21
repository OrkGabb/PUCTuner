// Offline critic replay from adaptive_dataset.csv rows the brain never saw.
//
// A dataset row carries the features before and after one learned pair, the move, and the scalar
// gain -- not the per-term costs the critic learns from. Those are rebuilt from the AFTER features,
// which is exactly how core.cpp's costs() and features() define them (checked against the logged
// gain on 7422 of 7422 rows, max error 1.1e-6; the same rebuild from the BEFORE features matched
// 0.01%). The discount is recovered from the chain: when a row's before-features are the previous
// row's after-features the pair continued from it and spans the gap between their `at`s;
// otherwise one 6 s window is assumed.
//
// Only the critic learns. Model cells need three raw observations and the policy prior needs the
// full context key and the TD error of the time; neither is in the file. `windows` is left alone:
// it counts windows this brain learned live.
//
//   replay_dataset <model in> <model out> <config> <at_lo> <at_hi> <dataset.csv>...
// With no dataset files it only loads and saves, which must reproduce the input byte for byte.
#include "../module/engine/platform.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

using namespace m54;

static bool parseRow(const std::string& line, double& at, int& tier, Features& before, Features& after) {
    std::vector<double> v;
    std::stringstream in(line);
    std::string cell;
    while (std::getline(in, cell, ',')) {
        char* end = nullptr;
        const double x = std::strtod(cell.c_str(), &end);
        if (end == cell.c_str() || !std::isfinite(x)) return false;
        v.push_back(x);
    }
    const size_t dim = before.size();
    if (v.size() != 5 + 2 * dim) return false;
    at = v[0]; tier = static_cast<int>(v[1]);
    for (size_t k = 0; k < dim; ++k) { before[k] = v[5 + k]; after[k] = v[5 + dim + k]; }
    return tier >= 0 && tier <= 2;
}

static CostVector rebuild(const Features& f) {
    CostVector k{};
    const double stall = f[22];
    k[LateCost] = f[20] == 0 ? f[1] : stall;
    k[JankCost] = f[2];
    k[EnergyCost] = f[3] * 1.5;
    k[PressureCost] = stall;
    k[HeatCost] = f[4] * 2;
    k[RisingCost] = f[5];
    k[BatteryCost] = f[6] * 2;
    k[BreachCost] = (k[HeatCost] >= 1 || k[BatteryCost] >= 1) ? 1 : 0;
    int effort = 0;
    for (int i = 0; i < Axes; ++i) effort += static_cast<int>(std::lround(f[14 + i] * (Levels - 1)));
    k[EffortCost] = effort / double(Axes * (Levels - 1));
    return k;
}

int main(int argc, char** argv) {
    if (argc < 6) { std::cerr << "usage: replay_dataset in out config at_lo at_hi [csv...]\n"; return 2; }
    const std::string stored = readText(argv[1], 4 * 1024 * 1024);
    std::istringstream header(stored);
    std::string version, identity;
    if (!(header >> version >> identity)) { std::cerr << "unreadable header\n"; return 3; }
    const auto cfg = readConfig(argv[3]);
    const auto rehome = legacyIdentities(cfg, configIdentity(cfg));
    Brain brain;
    if (!brain.deserialize(stored, identity, rehome)) { std::cerr << "brain does not load\n"; return 4; }
    const double lo = std::atof(argv[4]), hi = std::atof(argv[5]);

    std::map<double, std::pair<int, std::pair<Features, Features>>> rows;   // ordered, de-duplicated by `at`
    std::map<double, std::pair<int, std::pair<Features, Features>>> known;  // already learned: the control
    for (int i = 6; i < argc; ++i) {
        std::ifstream in(argv[i]);
        std::string line;
        while (std::getline(in, line)) {
            double at = 0; int tier = 0; Features b{}, a{};
            if (!parseRow(line, at, tier, b, a)) continue;
            if (at >= lo && at < hi) rows[at] = {tier, {b, a}};
            else if (at >= hi) known[at] = {tier, {b, a}};
        }
    }

    const uint32_t updatesBefore = brain.critic.updates;
    double sqBefore = 0, sqAfter = 0;
    size_t chained = 0;
    std::vector<std::tuple<Features, CostVector, Features, Tier, double>> pairs;
    double lastAt = -1; Features lastAfter{};
    for (const auto& [at, row] : rows) {
        const auto& [tier, fa] = row;
        const auto& [b, a] = fa;
        double step = Discount;
        bool same = lastAt >= 0 && at - lastAt <= 30;
        for (size_t k = 0; same && k < b.size(); ++k) same = std::abs(b[k] - lastAfter[k]) <= 1e-5 * (1 + std::abs(b[k]));
        if (same) { step = std::pow(Discount, (at - lastAt) / 6); ++chained; }
        pairs.emplace_back(b, rebuild(a), a, static_cast<Tier>(tier), step);
        lastAt = at; lastAfter = a;
    }
    auto tdSquare = [&brain](const auto& p) {
        const auto& [b, k, a, tier, step] = p;
        double target = 1;
        for (int i = 0; i < Costs; ++i) target -= 2 * preference(tier)[i] * std::clamp(k[i], 0., 4.);
        const double e = target + step * brain.critic.value(a, tier) - brain.critic.value(b, tier);
        return e * e;
    };
    std::vector<std::tuple<Features, CostVector, Features, Tier, double>> control;
    for (const auto& [at, row] : known)
        control.emplace_back(row.second.first, rebuild(row.second.second), row.second.second,
                             static_cast<Tier>(row.first), Discount);
    double controlBefore = 0, controlAfter = 0;
    for (const auto& p : control) controlBefore += tdSquare(p);
    for (const auto& p : pairs) sqBefore += tdSquare(p);
    for (const auto& p : pairs) {
        const auto& [b, k, a, tier, step] = p;
        brain.critic.learn(b, k, a, tier, 1, step);
    }
    for (const auto& p : pairs) sqAfter += tdSquare(p);
    for (const auto& p : control) controlAfter += tdSquare(p);

    if (!atomicText(argv[2], brain.serialize(identity))) { std::cerr << "cannot write output\n"; return 5; }
    Brain check;
    if (!check.deserialize(readText(argv[2], 4 * 1024 * 1024), identity, rehome) ||
        check.critic.updates != brain.critic.updates || check.windows != brain.windows ||
        check.model.samples != brain.model.samples || check.model.cells.size() != brain.model.cells.size()) {
        std::cerr << "output does not read back\n"; return 6;
    }
    const double n = std::max<size_t>(1, pairs.size());
    std::printf("rows %zu (chained %zu) critic_updates %u -> %u windows %llu cells %zu\n",
                pairs.size(), chained, updatesBefore, brain.critic.updates,
                static_cast<unsigned long long>(brain.windows), brain.model.cells.size());
    std::printf("mean squared TD error on the replayed rows: %.5f -> %.5f\n", sqBefore / n, sqAfter / n);
    const double m = std::max<size_t>(1, control.size());
    std::printf("control, %zu rows the brain already learned: %.5f -> %.5f\n",
                control.size(), controlBefore / m, controlAfter / m);
    return 0;
}
