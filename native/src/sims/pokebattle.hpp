// pokebattle.hpp — the Generation I battle system, and agents that learn it.
//
// A different KIND of problem from everything else on this bench. The maze,
// cart-pole and the platformer are deterministic control problems against
// physics. This is turn-based, stochastic, and adversarial: the damage roll is
// random, criticals are random, and there is an opponent choosing at the same
// time as you. What has to be learned is a PAYOFF STRUCTURE — which move beats
// which type — rather than a trajectory.
//
// MECHANICS, not assets. The Gen I damage formula and type chart are documented
// and long since reverse-engineered; they are rules, and rules are what this
// implements. There are no sprites, no music, no ROM and no emulator here. The
// species table is base stats, which are published facts, and it is small and
// declared in one place so it is obvious what it is.
//
// THE FORMULA, exactly as Gen I computes it:
//
//     damage = ((((2 * Level / 5 + 2) * Power * A / D) / 50) + 2) * STAB
//              * TypeEffectiveness * random(217..255) / 255
//
// with integer truncation at each division, which is why the self-test checks a
// hand-computed case rather than an approximation — a floating-point version of
// this formula gives different numbers, and "close enough" is how a rules
// engine ends up quietly wrong. TypeEffectiveness is written above as one
// factor and is not: Gen I applies one per defending type, in turn, truncating
// each time, so a dual type truncates twice. damage() says what that costs.
//
// Gen I specifics kept on purpose: one Special stat rather than the later split,
// criticals scaling with Speed, and the 217..255 damage roll.
//
// NOT modelled, and the list is worth having: there is no accuracy check, so
// every attack here connects. PMove carries an accuracy field, every move on
// the roster sets it to 100, and nothing reads it.

#pragma once
#include "../sim.hpp"
#include "../rng.hpp"
#include "../render/voxel.hpp"     // Surface
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

namespace bench {

// The fifteen Gen I types.
enum class PType { Normal, Fire, Water, Electric, Grass, Ice, Fighting, Poison,
                   Ground, Flying, Psychic, Bug, Rock, Ghost, Dragon, Count };

inline const char* type_name(PType t) {
    static const char* n[] = {"Normal","Fire","Water","Electric","Grass","Ice","Fighting",
                              "Poison","Ground","Flying","Psychic","Bug","Rock","Ghost","Dragon"};
    return n[int(t)];
}

// Effectiveness as a percentage, so the table is exact integers: 0, 50, 100, 200.
// Only the non-neutral entries are listed; everything else is 100.
inline int type_effect(PType atk, PType def) {
    struct E { PType a, d; int pct; };
    static const E table[] = {
        {PType::Fire,PType::Grass,200},{PType::Fire,PType::Ice,200},{PType::Fire,PType::Bug,200},
        {PType::Fire,PType::Water,50},{PType::Fire,PType::Fire,50},{PType::Fire,PType::Rock,50},
        {PType::Fire,PType::Dragon,50},
        {PType::Water,PType::Fire,200},{PType::Water,PType::Ground,200},{PType::Water,PType::Rock,200},
        {PType::Water,PType::Water,50},{PType::Water,PType::Grass,50},{PType::Water,PType::Dragon,50},
        {PType::Electric,PType::Water,200},{PType::Electric,PType::Flying,200},
        {PType::Electric,PType::Electric,50},{PType::Electric,PType::Grass,50},
        {PType::Electric,PType::Dragon,50},{PType::Electric,PType::Ground,0},
        {PType::Grass,PType::Water,200},{PType::Grass,PType::Ground,200},{PType::Grass,PType::Rock,200},
        {PType::Grass,PType::Fire,50},{PType::Grass,PType::Grass,50},{PType::Grass,PType::Poison,50},
        {PType::Grass,PType::Flying,50},{PType::Grass,PType::Bug,50},{PType::Grass,PType::Dragon,50},
        {PType::Ice,PType::Grass,200},{PType::Ice,PType::Ground,200},{PType::Ice,PType::Flying,200},
        {PType::Ice,PType::Dragon,200},{PType::Ice,PType::Water,50},{PType::Ice,PType::Ice,50},
        {PType::Fighting,PType::Normal,200},{PType::Fighting,PType::Ice,200},
        {PType::Fighting,PType::Rock,200},{PType::Fighting,PType::Poison,50},
        {PType::Fighting,PType::Flying,50},{PType::Fighting,PType::Psychic,50},
        {PType::Fighting,PType::Bug,50},{PType::Fighting,PType::Ghost,0},
        {PType::Poison,PType::Grass,200},{PType::Poison,PType::Bug,200},{PType::Poison,PType::Poison,50},
        {PType::Poison,PType::Ground,50},{PType::Poison,PType::Rock,50},{PType::Poison,PType::Ghost,50},
        {PType::Ground,PType::Fire,200},{PType::Ground,PType::Electric,200},
        {PType::Ground,PType::Poison,200},{PType::Ground,PType::Rock,200},
        {PType::Ground,PType::Grass,50},{PType::Ground,PType::Bug,50},{PType::Ground,PType::Flying,0},
        {PType::Flying,PType::Grass,200},{PType::Flying,PType::Fighting,200},
        {PType::Flying,PType::Bug,200},{PType::Flying,PType::Electric,50},
        {PType::Flying,PType::Rock,50},
        {PType::Psychic,PType::Fighting,200},{PType::Psychic,PType::Poison,200},
        {PType::Psychic,PType::Psychic,50},
        {PType::Bug,PType::Grass,200},{PType::Bug,PType::Poison,200},{PType::Bug,PType::Psychic,200},
        {PType::Bug,PType::Fire,50},{PType::Bug,PType::Fighting,50},{PType::Bug,PType::Flying,50},
        {PType::Bug,PType::Ghost,50},
        {PType::Rock,PType::Fire,200},{PType::Rock,PType::Ice,200},{PType::Rock,PType::Flying,200},
        {PType::Rock,PType::Bug,200},{PType::Rock,PType::Fighting,50},{PType::Rock,PType::Ground,50},
        {PType::Ghost,PType::Ghost,200},{PType::Ghost,PType::Normal,0},{PType::Ghost,PType::Psychic,0},
        {PType::Dragon,PType::Dragon,200},
        {PType::Normal,PType::Rock,50},{PType::Normal,PType::Ghost,0},
    };
    for (const auto& e : table) if (e.a == atk && e.d == def) return e.pct;
    return 100;
}

// accuracy is the Gen I percentage. It is stored and never read: nothing here
// rolls to hit. See the NOT-modelled note at the top of the file.
struct PMove { std::string name; PType type; int power; int accuracy; bool special; };
struct PSpecies {
    std::string name;
    PType t1, t2;                       // t2 == Count when single-typed
    int hp, atk, def, spc, spd;         // Gen I base stats: one Special
    std::vector<PMove> moves;
};

class PokeBattle final : public Sim {
public:
    static constexpr int kLevel = 50;

    PokeBattle() {
        about_ = Provenance{
            "Gen I battle system, learned", "1996",
            "battle mechanics as documented and reverse-engineered by the community",
            "Damage formula and type chart per the Generation I mechanics; the implementation "
            "here is checked against hand-computed values in the self-test",
            Replication::No,
            "No. Agents learn a payoff structure; nothing copies itself.",
            "Turn-based, stochastic and adversarial - a different kind of problem from the "
            "physics tasks on this bench. Damage is rolled, criticals are rolled, and both "
            "sides choose at once. What has to be learned is WHICH MOVE BEATS WHICH TYPE, and "
            "the agent is measured against two baselines: random choice, and always firing the "
            "highest-power move regardless of type."
        };
        pal_ = { {{8,11,14},"empty"}, {{90,209,196},"learner"}, {{242,193,78},"greedy baseline"},
                 {{217,83,79},"random baseline"} };
        knobs_ = {
            {"seed", "run seed", 1.f, 40.f, 1.f, 1.f, {}, true,
             "Which run this is. The same seed reproduces the identical run; different "
             "seeds are independent runs of the SAME configuration. Every measurement in "
             "this project needed several — a single run cannot tell a real effect from a "
             "lucky one."},
            {"alpha", "learning rate", 0.01f, 1.0f, 0.25f, 0.01f, {}, false,
             "How fast the learner updates its estimate of each move's value against each "
             "defending type."},
            {"epsilon", "exploration", 0.0f, 1.0f, 0.20f, 0.01f, {}, false,
             "Chance of ignoring its table and picking at random."},
            {"battles", "battles per tick", 1.f, 200.f, 20.f, 1.f, {}, false, "Display rate only.", true},
        };
        surf_.resize(kW, kH);
        view_ = Field(64, 32);
        buildRoster();
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_; }
    const Field&               field()   const override { return view_; }
    const Surface*             surface() const override { return &surf_; }
    std::uint64_t              generation() const override { return gen_; }
    std::vector<Knob>&         knobs()   override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "epsilon") eps_ = v;
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[176];
        std::snprintf(b, sizeof b,
            "%d battles  ·  learner %.0f%%  ·  greedy %.0f%%  ·  random %.0f%%",
            battles_, 100.0 * winRate(wonL_), 100.0 * winRate(wonG_), 100.0 * winRate(wonR_));
        return b;
    }

    void reset() override {
        rng_.reseed(runSeed(seed_));
        q_.assign(roster_.size() * kMoves * std::size_t(PType::Count), 0.0f);
        battles_ = wonL_ = wonG_ = wonR_ = 0;
        eps_ = knob("epsilon");
        histL_.clear(); histG_.clear(); histR_.clear();
        gen_ = 0;
        publish();
    }

    void step() override {
        const int n = std::max(1, int(knob("battles") + 0.5f));
        for (int i = 0; i < n; ++i) {
            wonL_ += playBattle(Agent::Learner) ? 1 : 0;
            wonG_ += playBattle(Agent::Greedy)  ? 1 : 0;
            wonR_ += playBattle(Agent::Random)  ? 1 : 0;
            ++battles_;
            if ((battles_ & 15) == 0) {
                histL_.push_back(winRate(wonL_));
                histG_.push_back(winRate(wonG_));
                histR_.push_back(winRate(wonR_));
                if (histL_.size() > 600) {
                    histL_.erase(histL_.begin()); histG_.erase(histG_.begin());
                    histR_.erase(histR_.begin());
                }
            }
        }
        ++gen_;
        publish();
    }

    std::vector<Metric> metrics() const override {
        return {
            Metric{ "learner win rate", winRate(wonL_), 1.0 },
            Metric{ "greedy baseline",  winRate(wonG_), 1.0 },
            Metric{ "random baseline",  winRate(wonR_), 1.0 },
        };
    }

    // Run the whole experiment again on different luck: a new seed, an empty
    // table, and every counter back to zero.
    //
    // The re-seed is the point of it. reset() would rebuild the same run from
    // the same seed and tell you nothing new, whereas this asks the one
    // question a single run cannot answer — whether a win rate was a property
    // of the agent or of one sequence of dice.
    bool poke(float, float) override {
        seed_ += 0x9E3779B9ull;
        rng_.reseed(seed_);
        std::fill(q_.begin(), q_.end(), 0.0f);
        battles_ = wonL_ = wonG_ = wonR_ = 0;
        histL_.clear(); histG_.clear(); histR_.clear();
        publish();
        return true;
    }

    // One epoch is 100 battles: a single battle is a coin flip and tells you
    // nothing, so the unit that matters is a block big enough to move a rate.
    [[nodiscard]] const char* epoch_name() const override { return "100 battles"; }
    [[nodiscard]] int epoch_count() const override { return battles_ / 100; }
    bool advance_epoch() override { run(100); publish(); return true; }

    // ── the damage formula, exposed so it can be checked ───────────────────
    //
    // Integer truncation at every division, exactly as Gen I does it. `roll` is
    // the 217..255 damage variance; pass 255 for the maximum, which is what the
    // hand-computed test case uses.
    static int damage(int level, const PMove& m, const PSpecies& atk, const PSpecies& def,
                      int roll, bool critical) {
        if (m.power <= 0) return 0;
        const int A = m.special ? atk.spc : atk.atk;
        const int D = m.special ? def.spc : def.def;
        const int lvl = critical ? level * 2 : level;
        int d = ((2 * lvl / 5 + 2) * m.power * A / D) / 50 + 2;
        // STAB: 1.5x, applied as *3/2 with truncation.
        if (m.type == atk.t1 || (atk.t2 != PType::Count && m.type == atk.t2)) d = d * 3 / 2;
        // Each defending type in TURN, truncating on each one.
        //
        // This used to multiply the two effectivenesses together and divide
        // once, which is a different sum. Gen I walks the type table and does
        // one multiply-then-divide per matching defending type, so a dual type
        // truncates twice. Measured over every roster attacker, move, defender,
        // roll and critical — 11232 cases — the two orders disagree on 115 of
        // them, all of them dual-typed defenders and all by exactly 1 HP: the
        // Fire-type's Earthquake on Overgrow (Grass halves Ground, Poison
        // doubles it) at roll 217 with a critical gave 74 folded and gives 73
        // now. Small, and this file's claim is that it computes what Gen I
        // computes, not that it is close.
        //
        // The table is in percent where Gen I is in tenths, which changes
        // nothing: 50/100 and 5/10 truncate to the same integer.
        d = d * type_effect(m.type, def.t1) / 100;
        if (def.t2 != PType::Count) d = d * type_effect(m.type, def.t2) / 100;
        if (d == 0) return 0;
        d = d * roll / 255;
        return std::max(1, d);
    }

    // ── measurement ────────────────────────────────────────────────────────
    [[nodiscard]] double learner_rate() const { return winRate(wonL_); }
    [[nodiscard]] double greedy_rate()  const { return winRate(wonG_); }
    [[nodiscard]] double random_rate()  const { return winRate(wonR_); }
    [[nodiscard]] int    battles() const { return battles_; }
    void run(int battles) { for (int i = 0; i < battles; ++i) {
            wonL_ += playBattle(Agent::Learner) ? 1 : 0;
            wonG_ += playBattle(Agent::Greedy)  ? 1 : 0;
            wonR_ += playBattle(Agent::Random)  ? 1 : 0;
            ++battles_;
        } }
    [[nodiscard]] const std::vector<PSpecies>& roster() const { return roster_; }

private:
    enum class Agent { Learner, Greedy, Random };
    static constexpr int kMoves = 4, kW = 720, kH = 380;

    // Which independent run this is. Same seed, same run; different seed, an
    // independent sample of the same configuration.
    [[nodiscard]] std::uint64_t runSeed(std::uint64_t base) const {
        return mix_seed(base, int(knob("seed") + 0.5f));
    }
    [[nodiscard]] float knob(const char* k) const {
        for (auto& kn : knobs_) if (kn.key == k) return kn.value;
        return 0.f;
    }
    [[nodiscard]] double winRate(int won) const {
        return battles_ ? double(won) / double(battles_) : 0.0;
    }

    // Base stats are published facts. A small roster, declared in one place, so
    // it is obvious exactly what data this uses and how little of it there is.
    void buildRoster() {
        // The 100 is accuracy — stored, and read nowhere. Every move on this
        // roster is a hundred-percenter in any case.
        auto mv = [](const char* n, PType t, int p, bool sp) { return PMove{n, t, p, 100, sp}; };
        roster_ = {
            {"Ember-type",   PType::Fire,     PType::Count, 78, 84, 78, 85, 100,
             { mv("Flamethrower", PType::Fire, 95, true), mv("Slash", PType::Normal, 70, false),
               mv("Earthquake", PType::Ground, 100, false), mv("Bite", PType::Normal, 60, false) }},
            {"Torrent-type", PType::Water,    PType::Count, 79, 83, 100, 85, 78,
             { mv("Surf", PType::Water, 95, true), mv("Ice Beam", PType::Ice, 95, true),
               mv("Body Slam", PType::Normal, 85, false), mv("Rock Slide", PType::Rock, 75, false) }},
            {"Overgrow",     PType::Grass,    PType::Poison, 80, 82, 83, 100, 80,
             { mv("Razor Leaf", PType::Grass, 55, true), mv("Sludge", PType::Poison, 65, true),
               mv("Body Slam", PType::Normal, 85, false), mv("Take Down", PType::Normal, 90, false) }},
            {"Static",       PType::Electric, PType::Count, 35, 55, 40, 50, 90,
             { mv("Thunderbolt", PType::Electric, 95, true), mv("Quick Attack", PType::Normal, 40, false),
               mv("Iron Tail", PType::Normal, 70, false), mv("Dig", PType::Ground, 80, false) }},
            {"Boulder",      PType::Rock,     PType::Ground, 80, 110, 130, 55, 45,
             { mv("Rock Slide", PType::Rock, 75, false), mv("Earthquake", PType::Ground, 100, false),
               mv("Body Slam", PType::Normal, 85, false), mv("Fire Blast", PType::Fire, 120, true) }},
            {"Wingspan",     PType::Flying,   PType::Normal, 83, 80, 75, 70, 101,
             { mv("Drill Peck", PType::Flying, 80, false), mv("Body Slam", PType::Normal, 85, false),
               mv("Steel Wing", PType::Normal, 70, false), mv("Sky Attack", PType::Flying, 140, false) }},
        };
    }

    [[nodiscard]] int maxHP(const PSpecies& s) const {
        // Gen I HP at level L with no EVs/IVs: ((base + 0) * 2 * L)/100 + L + 10
        return (s.hp * 2 * kLevel) / 100 + kLevel + 10;
    }

    // Indexed by WHO IS ATTACKING as well as which slot and which defender.
    //
    // Indexing by slot alone was a real bug and a quiet one: move 0 is
    // Flamethrower for one species and Surf for another, so a single table
    // entry was averaging the value of completely different moves. Measured at
    // the defaults over 20000 battles, it left the learner at 55.2% —
    // indistinguishable from random (55.3%) and well behind the greedy baseline
    // (61.1%). The state has to identify the move, not its position in a list.
    [[nodiscard]] int qIndex(int attacker, int move, PType defType) const {
        return (attacker * kMoves + move) * int(PType::Count) + int(defType);
    }
    [[nodiscard]] int speciesIndex(const PSpecies& s) const {
        for (std::size_t i = 0; i < roster_.size(); ++i) if (&roster_[i] == &s) return int(i);
        return 0;
    }

    int chooseMove(Agent who, const PSpecies& me, const PSpecies& them) {
        if (who == Agent::Random) return int(rng_.unit() * float(kMoves)) % kMoves;
        if (who == Agent::Greedy) {
            // The obvious-looking baseline: always fire the biggest number,
            // ignoring type entirely. Beating THIS is the thing that shows the
            // learner has picked up the payoff structure rather than just
            // learned to attack.
            int best = 0;
            for (int m = 1; m < kMoves; ++m)
                if (me.moves[std::size_t(m)].power > me.moves[std::size_t(best)].power) best = m;
            return best;
        }
        if (rng_.unit() < eps_) return int(rng_.unit() * float(kMoves)) % kMoves;
        const int me_i = speciesIndex(me);
        int best = 0;
        for (int m = 1; m < kMoves; ++m)
            if (q_[std::size_t(qIndex(me_i, m, them.t1))]
              > q_[std::size_t(qIndex(me_i, best, them.t1))]) best = m;
        return best;
    }

    // One battle to knockout. Both sides are drawn at random from the roster,
    // so a win rate is over the whole matchup space rather than one pairing.
    bool playBattle(Agent who) {
        const PSpecies& me   = roster_[std::size_t(rng_.unit() * float(roster_.size())) % roster_.size()];
        const PSpecies& them = roster_[std::size_t(rng_.unit() * float(roster_.size())) % roster_.size()];
        int myHP = maxHP(me), theirHP = maxHP(them);
        const bool meFirst = me.spd >= them.spd;

        for (int turn = 0; turn < 60; ++turn) {
            const int mv = chooseMove(who, me, them);
            const PMove& mine = me.moves[std::size_t(mv)];
            const int roll = 217 + int(rng_.unit() * 39.0f);
            const bool crit = rng_.unit() < float(me.spd) / 512.0f;
            const int dealt = damage(kLevel, mine, me, them, roll, crit);

            // The opponent always plays greedily, so the learner is up against
            // a fixed, competent policy rather than a moving target.
            int omv = 0;
            for (int m = 1; m < kMoves; ++m)
                if (them.moves[std::size_t(m)].power > them.moves[std::size_t(omv)].power) omv = m;
            const PMove& theirs = them.moves[std::size_t(omv)];
            const int oroll = 217 + int(rng_.unit() * 39.0f);
            const int taken = damage(kLevel, theirs, them, me, oroll, false);

            const int me_i = speciesIndex(me);
            auto learn = [&](int reward) {
                if (who != Agent::Learner) return;
                float& q = q_[std::size_t(qIndex(me_i, mv, them.t1))];
                q += knob("alpha") * (float(reward) - q);
            };

            if (meFirst) {
                theirHP -= dealt;
                if (theirHP <= 0) { learn(1); return true; }
                myHP -= taken;
                if (myHP <= 0) { learn(-1); return false; }
            } else {
                myHP -= taken;
                if (myHP <= 0) { learn(-1); return false; }
                theirHP -= dealt;
                if (theirHP <= 0) { learn(1); return true; }
            }
        }
        return theirHP < myHP;
    }

    // ── drawing ────────────────────────────────────────────────────────────
    void px(int x, int y, Rgb c) {
        if (x < 0 || y < 0 || x >= kW || y >= kH) return;
        std::uint8_t* p = &surf_.rgba[(std::size_t(y)*kW + x)*4];
        p[0]=c.r; p[1]=c.g; p[2]=c.b; p[3]=255;
    }
    void box(int x0,int y0,int x1,int y1,Rgb c){ for(int y=y0;y<y1;++y) for(int x=x0;x<x1;++x) px(x,y,c); }
    void plot(const std::vector<double>& v, Rgb c, int L, int T, int W, int H) {
        if (v.size() < 2) return;
        const double n = double(v.size() - 1);
        int px0=-1, py0=-1;
        for (std::size_t i = 0; i < v.size(); ++i) {
            const int x = L + int(double(W) * double(i) / n);
            const int y = T + H - int(double(H) * std::clamp(v[i], 0.0, 1.0));
            if (px0 >= 0) {
                const int st = std::max(1, std::max(std::abs(x-px0), std::abs(y-py0)));
                for (int k=0;k<=st;++k) px(px0+(x-px0)*k/st, py0+(y-py0)*k/st, c);
            }
            px0=x; py0=y;
        }
    }

    void publish() {
        for (int i=0;i<kW*kH;++i){ std::uint8_t* p=&surf_.rgba[std::size_t(i)*4];
            p[0]=12;p[1]=15;p[2]=20;p[3]=255; }

        // The learned table: rows are moves, columns defending types, brightness
        // is the learned value. This is the payoff structure being discovered.
        const int cw = 22, ch = 26, L = 40, T = 40;
        for (int m = 0; m < kMoves; ++m)
            for (int t = 0; t < int(PType::Count); ++t) {
                const float q = q_[std::size_t(qIndex(0, m, PType(t)))];
                const float v = std::clamp(q * 0.5f + 0.5f, 0.0f, 1.0f);
                box(L + t*cw, T + m*ch, L + (t+1)*cw - 2, T + (m+1)*ch - 2,
                    Rgb{ std::uint8_t(24 + 200*v), std::uint8_t(30 + 190*v),
                         std::uint8_t(40 + 150*v) });
            }
        // Win-rate curves beneath, with the 50% line marked.
        const int PT = T + kMoves*ch + 40, PH = 150, PW = kW - L - 30;
        for (int x = L; x < L + PW; x += 3) px(x, PT + PH/2, Rgb{70,80,92});
        for (int y = PT; y < PT + PH; ++y) px(L, y, Rgb{60,74,88});
        plot(histR_, Rgb{217,83,79},  L, PT, PW, PH);
        plot(histG_, Rgb{242,193,78}, L, PT, PW, PH);
        plot(histL_, Rgb{90,209,196}, L, PT, PW, PH);

        view_.fill(0);
        for (int i = 0; i < view_.w && !histL_.empty(); ++i) {
            const std::size_t k = std::size_t(double(i)/double(view_.w-1)*double(histL_.size()-1));
            auto band = [&](const std::vector<double>& h, std::uint8_t idx) {
                const int y = std::clamp(int((1.0 - h[k]) * double(view_.h - 1)), 0, view_.h-1);
                view_.set(i, y, idx);
            };
            band(histR_, 3); band(histG_, 2); band(histL_, 1);
        }
    }

    Provenance about_;
    std::vector<Swatch> pal_;
    std::vector<Knob>   knobs_;
    Field   view_;
    Surface surf_;
    std::vector<PSpecies> roster_;
    std::vector<float>    q_;
    std::vector<double>   histL_, histG_, histR_;
    int   battles_ = 0, wonL_ = 0, wonG_ = 0, wonR_ = 0;
    float eps_ = 0.2f;
    std::uint64_t seed_ = 0xB4771Eull;
    Rng   rng_{0xB4771Eull};
    std::uint64_t gen_ = 0;
};

inline SimPtr make_pokebattle() { return std::make_unique<PokeBattle>(); }

} // namespace bench
