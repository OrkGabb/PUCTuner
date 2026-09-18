#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <random>
#include <string>
#include <vector>

namespace m54 {
enum class Tier { Game, Balanced, Powersave };
// A single, fixed objective in production. Legacy tiers remain readable in saved brains and
// offline comparisons; they are never selected from an app name or a user's activity label.
constexpr Tier RuntimeObjective = Tier::Balanced;
constexpr int Axes = 4;
constexpr int Levels = 5;
// Relative moves, not absolute setpoints: stay, one step per axis/direction, full release.
constexpr int Moves = 2 + 2 * Axes;
constexpr int Stay = 0, Release = Moves - 1;
constexpr double Discount = .88;                  // ~48 s horizon at one 6 s window per step
constexpr double ValueLimit = 1 / (1 - Discount); // bound of any discounted return

// Independent axes, not three performance presets: CPU floor, GPU window, MIF floor, PELT.
struct Action {
    std::array<int, Axes> level{};
    int id() const;
    static Action fromId(int id);
    int effort() const;
    bool operator==(const Action& a) const { return level == a.level; }
    bool operator!=(const Action& a) const { return !(*this == a); }
};
struct Observation {
    double at = 0, cpu = 0, gpu = 0, cpuPsi = 0, memPsi = 0, ioPsi = 0;
    long memAvailKb = 0;
    // Swap occupancy from /proc/meminfo. Telemetry only: MemAvailable read 2.3 GB while 3.5 of
    // 4 GB of zram was occupied (2026-09-14, after opening many apps on purpose). Whether that
    // occupancy costs anything on this device is a question for measured windows, and it must
    // never trigger process kills -- a full cache of routine apps is what spares cold starts.
    long swapTotalKb = 0, swapFreeKb = 0;
    // Busiest single core, and busiest single thread of the foreground app, both as a fraction
    // of one core. Aggregate utilisation cannot see a saturated thread: one core pinned at 100%
    // of eight reads as 12.5% overall, which is what a CPU-bound game looks like to a controller
    // watching only the average, and it looks like plenty of headroom. The per-core figure still
    // understates it, because a hot thread that migrates shows as a moderate load on several
    // cores instead of a pinned one; only the per-thread figure catches that.
    double cpuPeak = 0, threadPeak = 0;
    // Paging, as rates per second over the window, from /proc/vmstat. Measured and exported,
    // never in the objective -- the same terms the queue channel is on. The reason it needs its
    // own channel at all is that `memPsi` cannot see this pathology: a zram refault is an lz4
    // decompression of tens of microseconds, so it spends CPU rather than stalling on memory,
    // and PSI memory read avg10=0.00 on this device through 618 major faults and 14 500 file
    // refaults a second. Whether that costs frames HERE is a question for the collected windows,
    // not for a weight picked in advance.
    double majorFaults = 0, swapIn = 0, fileRefault = 0;
    bool pagingValid = false;
    // Junction temperature: the hottest of the BIG, LITTLE and G3D die sensors, and the name of
    // whichever one it was. Not skin temperature -- at rest here the die sensors read 30-35 C
    // while the battery zone reads 28.7, and under load that gap widens to tens of degrees, so
    // this number is not what a hand on the glass feels and must not be compared with it.
    double temp = 0, batteryTemp = 0, trend = 0, watts = 0, energy = 0;
    std::string hotZone;
    double p95 = 0, jank = 0;
    // Presentation intervals actually counted, independent of controller mode and poll timing.
    double frameStart = 0, frameEnd = 0, frameTimeMs = 0;
    int slowFrames50 = 0;
    // Runqueue delay of the foreground app's threads, in ms, from the eBPF probe. Measured and
    // exported, not yet part of the reward: a new signal has to demonstrate correlation with
    // observed stutter on real data before it is allowed to move a decision.
    double queueMs = 0, queuePeakMs = 0, queueLate = 0;
    bool queueValid = false;
    int frames = 0, battery = 0, target = 60;
    // What this window alone proposed as the cadence, before stickiness. Exported so the gap
    // between proposal and adopted target is measurable instead of argued about: the hysteresis
    // constants below are only defensible while that gap is small.
    int cadenceSeen = 0;
    bool awake = false, charging = false, thermalValid = false, powerValid = false;
    bool framesValid = false, loadValid = false;
    std::string app;
};
struct Constraints {
    Tier tier = Tier::Balanced;
    std::array<bool, Axes> allowed{true, true, true, true};
    double high = 75, batteryHigh = 43;
    int ceiling = Levels - 1; // thermal-energy budget cap, refilled while cool or idle
};
struct Choice {
    int move = Stay;
    Action action;
};
// Fine context drives residuals; coarse is their backoff parent. Neither carries the profile:
// what one step of GPU floor does to p95 in an app is physics of this silicon, and physics does
// not change because the user prefers battery today. Only `policy` carries it, because a
// preference over moves genuinely differs between objectives.
struct ContextKey {
    std::string fine, coarse, policy;
};
using Metrics = std::array<double, 5>;
// The objective, taken apart. Every term is a property of the measured window and of the action
// in effect -- none of them depends on which profile is selected. Only the WEIGHTS do. Writing
// it this way is what lets one measured window teach all three objectives at once instead of
// one, which is the difference between a controller that learns and three that each wait their
// turn: on this device, Economia had 0 critic updates after 540 windows because it had simply
// never been selected while something was being measured.
constexpr int Costs = 9;
enum CostTerm {
    LateCost = 0,   // unserved demand, from the deficit channels
    JankCost,       // share of intervals past the cadence budget
    EnergyCost,     // measured watts, or the clock/load proxy
    PressureCost,   // PSI, as a cost rather than as the deficit
    HeatCost,       // compute temperature above the working band
    RisingCost,     // how fast it is climbing
    BatteryCost,    // battery temperature above its band
    BreachCost,     // hard violation: over a limit, or the sensors cannot be trusted
    EffortCost,     // minimal-intervention toll on the action itself
};
using CostVector = std::array<double, Costs>;
// r(s, a) = 1 - 2 * (preference(tier) . costs(s, c, a)), clipped. The clip binds in 3 of 1530
// measured windows on this device (0.2%), so treating the reward as exactly linear in these
// terms is sound, and that linearity is the whole reason the decomposition below works.
CostVector costs(const Observation& s, const Constraints& c, Action applied);
const CostVector& preference(Tier tier);
// Service deficit: how much of the demand a window placed on the device went unserved. Three
// independent estimators, each carrying its own validity, because "not measured" and "measured
// zero" are different facts about the world. Collapsing them is what made a frameless minute --
// a download, a compile, a sync, a screen-off workload -- read to this controller as a workload
// being served perfectly. Frame lateness is one estimator of the deficit, not its definition.
constexpr int Channels = 3;
enum Channel { FrameChannel = 0, QueueChannel = 1, StallChannel = 2 };
struct Deficit {
    std::array<double, Channels> value{};
    std::array<bool, Channels> valid{};
    bool any() const;
    // The estimator the objective is currently allowed to read. Frames when a renderer is
    // producing them, otherwise stall. The queue channel is measured and carried into the
    // features, never into the objective, until it correlates with observed stutter here.
    double primary() const;
};
Deficit deficit(const Observation& s);
// Was the device asked to do anything at all this window? Deep idle is the one region of the
// state space a tuning controller never has to reason about, and at one window every six seconds
// it would otherwise become most of what the critic ever sees.
bool demanding(const Observation& s);
// Sound enough to learn from: sensors readable, some demand present, some deficit channel valid.
// Deliberately says nothing about frames, screen state or thermal safety -- those govern whether
// the engine may ACT, which is a separate permission.
bool measurable(const Observation& s);

using Features = std::array<double, 27>;
// Feature count before the regime channels were added. Weight k kept its meaning across that
// change, so an older brain loads by zero-padding instead of being thrown away.
constexpr size_t LegacyFeatures = 20;
// Same argument, one migration later: the width before the paging channels. Every weight index
// below this kept its meaning, so a brain saved at that width is read narrow and zero-padded,
// and 4000 windows of measured physics survive the schema change.
constexpr size_t PagingFreeFeatures = 25;

struct Experience {
    unsigned count = 0;
    uint64_t touched = 0;
    Metrics mean{}, variance{};
};
struct Decision {
    Action action;
    int move = Stay, simulations = 0, nodes = 1, depth = 0;
    double value = 0, prior = 0;
};

uint64_t hash(const std::string& text);
// Parses one persisted gradient scale (see core.cpp): strict on malformed text,
// NaN/inf and overflow, tolerant only of denormal underflow, which reads as 0.0.
bool parseScaleToken(const std::string& token, double& out);
ContextKey context(const Observation& s, const Constraints& c, const std::string& configId);
// Three widths of credit, from strictest to loosest: a residual blames one applied edge in one
// app; a policy preference belongs to one app; a value backup only needs two sound windows.
bool learnable(const Observation& before, const Observation& after);
bool attributable(const Observation& before, const Observation& after);
bool backupable(const Observation& before, const Observation& after);
double reward(const Observation& s, const Constraints& c);
double reward(const Observation& s, const Constraints& c, Action applied);
Features features(const Observation& s, const Constraints& c, Action applied);
Action safe(Action a, const Observation& s, const Constraints& c);
std::vector<Choice> candidates(Action current, const Observation& s, const Constraints& c);
int moveIndex(Action from, Action to, const Observation& s, const Constraints& c);

// Thermal-energy allowance. Sustained effort only costs while the device is already warm, so
// gaming on a cool phone is never throttled by the controller itself, and a long hot session
// loses headroom gradually instead of relying only on the reactive thermal cutoff.
class Budget {
    double level = 1;
public:
    double remaining() const { return level; }
    void restore(double value);
    void update(Action applied, const Observation& s, const Constraints& c, double seconds);
    // Time the device spent suspended, which update() cannot be handed: that one bounds a window
    // to 60 s, and a deep sleep arrives as one gap of minutes rather than as the two hundred
    // windows it would have been. Nothing ran and nothing heated, so this only refills -- and it
    // refills against the temperature measured on the way out, which after a long sleep is
    // ambient. Without it the allowance is frozen at whatever it held when the phone was put
    // down, and ceiling() then clamps every axis of a cold device as if it were still hot.
    void relax(const Observation& s, const Constraints& c, double seconds);
    int ceiling() const;
};

// Linear predictor of the discounted future of EACH cost term, not of one profile's scalar
// return. Since r = 1 - 2*(w_tier . costs), the value of any profile is a dot product away:
//
//     V_tier(s) = ValueLimit - 2 * ( preference(tier) . psi(s) )
//
// so one measured window updates every profile at once and no profile can be left untrained.
// It also carries nine learning targets per window instead of one, on a device that supplies a
// few hundred windows a session. The previous design kept three disjoint weight banks; measured
// consequence on this phone: Game 246 updates, Balanceado 1259, Economia 0.
class Critic {
public:
    static constexpr size_t Dim = std::tuple_size<Features>::value;
    static constexpr double MaxStep = .04; // hard bound on what one window may move a weight
    std::array<std::array<double, Dim>, Costs> weights{};
    std::array<std::array<double, Dim>, Costs> scale{}; // per-term gradient scale, persisted
    uint32_t updates = 0;
    double component(const Features& f, int term) const;
    double value(const Features& f, Tier tier) const;
    double value(const Observation& s, const Constraints& c, Action applied) const;
    double trust() const;
    // Returns the TD error of the SELECTED profile's value, which is the advantage handed to the
    // policy -- identical in meaning to before, now assembled from the per-term errors. `step` is
    // the discount for THIS pair: a backup that spans two windows because one was unusable must
    // discount twice, or the critic quietly learns that waiting is free.
    double learn(const Features& before, const CostVector& measured, const Features& after,
                 Tier tier, double rate = 1, double step = Discount);
};

// Softmax preference over relative moves: a tier-wide term plus a per-context correction,
// mixed with a uniform floor so exploration never collapses onto a single move.
class Prior {
public:
    static constexpr size_t MaxContexts = 512;
    static constexpr double Floor = .15;
    struct Cell {
        std::array<float, Moves> logit{};
        uint64_t touched = 0;
    };
    std::array<std::array<float, Moves>, 3> tier{};
    std::map<std::string, Cell> contexts;
    uint64_t updates = 0;
    std::vector<double> distribution(const std::string& coarse, Tier t,
                                     const std::vector<Choice>& options) const;
    void reinforce(const std::string& coarse, Tier t, int move,
                   const std::vector<Choice>& options, double advantage);
};

// Empirical model of transition residuals with a coarse backoff parent, so a cold fine cell
// inherits what the same app already taught at a coarser resolution. Search never writes here.
class Model {
public:
    static constexpr size_t MaxEntries = 3072;
    // Observations a coarse cell must hold before its fine children may be opened. Four is where
    // predict() starts trusting a cell at all -- the trust weight is evidence/(evidence+4) -- so
    // below it a fine cell could not have influenced a prediction even if it existed, and all it
    // did was occupy a slot and halve the evidence its parent would otherwise have accumulated.
    static constexpr unsigned FinePromotion = 4;
    std::map<std::string, Experience> cells;
    uint64_t samples = 0;
    uint64_t surprises = 0; // current process; evidence counts themselves are persisted
    Observation predict(const ContextKey& key, const Observation& s, Action from,
                        Action to, std::mt19937* random = nullptr) const;
    bool observe(const ContextKey& key, const Observation& before, Action from,
                 Action to, const Observation& after);
    unsigned count(const ContextKey& key, Action from, Action to) const;
};

// Acceptance gate on top of the search. Raising effort must clear a real modelled margin;
// releasing it needs only a hint, because the cheaper setting is the safe default and a wrong
// release costs one window while a wrong boost costs heat and charge for as long as it holds.
bool accept(const Model& model, const ContextKey& key, const Observation& s, Action current,
            Action next, const Constraints& c, bool explore);

// Why the controller did not spend more, recorded per window so the question can be answered
// from the history instead of argued from the code.
//
// "The CPU axis never went above level 1 in 569 windows" has two opposite explanations and the
// history could not tell them apart: the model may have LEARNED that raising the floor buys
// nothing here -- which on a device whose big-cluster ceiling is externally capped two thirds of
// the time may simply be true -- or it may never have been allowed to find out. Those call for
// opposite fixes, so the distinguishing facts belong in the file: what the model thinks the best
// costlier move is worth, what it had to beat, and how many times that edge has ever been
// measured in this context. An advantage near zero backed by `tried` in the dozens is a learned
// refusal; the same advantage with `tried` at zero is an untested guess.
struct Ambition {
    int move = -1;            // best costlier candidate, or -1 when the search offered none
    double advantage = 0;     // its modelled advantage over staying put
    double toll = 0;          // the margin it had to clear
    unsigned tried = 0;       // measured windows behind that edge in this context
    bool accepted = false;    // whether it would pass accept() right now
};
Ambition ambition(const Model& model, const ContextKey& key, const Observation& s,
                  Action current, const Constraints& c, bool explore);

// Bounded replay of real measured windows. Idle time is spent re-fitting the critic on this
// buffer instead of on imagined data, so learning continues without acting on the device.
class Replay {
public:
    static constexpr size_t Capacity = 256;
    struct Sample {
        Features before{}, after{};
        CostVector cost{};
        double step = Discount;
        uint8_t tier = 1;
    };
    std::vector<Sample> samples;
    uint64_t seen = 0;
    void add(const Features& before, const CostVector& measured, const Features& after, Tier tier,
             double step, std::mt19937& rng);
    // Returns the number of critic updates actually performed.
    int rehearse(Critic& critic, std::mt19937& rng, int steps);
};

class Brain {
public:
    Model model;
    Critic critic;
    Prior prior;
    Replay replay;
    uint64_t windows = 0;
    double budget = 1;
    std::string serialize(const std::string& identity) const;
    // `rehome` maps a stored context identity written by an older build onto the current one.
    // A key whose identity is absent from the map is dropped rather than guessed at: it was
    // measured on a tuning surface this build cannot reconstruct.
    bool deserialize(const std::string& data, const std::string& identity,
                     const std::map<std::string, std::string>& rehome = {});
};

class Planner {
    std::mt19937 rng;
public:
    explicit Planner(uint32_t seed) : rng(seed) {}
    std::mt19937& random() { return rng; }
    // PUCT selection over relative moves, model-based expansion with predictive thermal
    // pruning, truncated rollout bootstrapped by the critic, then backup.
    Decision search(const Brain& brain, const ContextKey& key, const Observation& s,
                    Action current, const Constraints& limits, bool explore,
                    int iterations = 128, int horizon = 4, int budgetMicros = 8000);
};
} // namespace m54
