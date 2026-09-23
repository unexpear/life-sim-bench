// von_neumann.hpp — the complete 29-state universal-constructor automaton.
//
// This is the machine Conway's rule was a deliberate simplification OF, so it
// belongs next to Life rather than in an appendix.
//
// States: 1 quiescent
//         8 ordinary transmission (4 directions x excited/quiescent)
//         8 special  transmission (4 directions x excited/quiescent)
//         4 confluent (a two-bit delay line)
//         8 sensitized (a binary tree walked during construction)
//        29 total.
//
// Transmission states OR their inputs. The confluent ANDs its ordinary inputs
// and holds two bits. Special kills ordinary, ordinary kills special, special
// kills confluent.
//
// Construction: a QUIESCENT cell struck by a pulse train walks the sensitized
// tree, one level per tick, and crystallises into whichever component the code
// named. All nine leaves:
//     10000 ord E   10001 ord N   1001 ord W   1010 ord S
//     1011  spc E   1100  spc N   1101 spc W   1110 spc S
//     1111  confluent
// You build matter by talking to empty space.
//
// ── the tape ────────────────────────────────────────────────────────────────
//
// What ran here before was a construction ARM: one hardcoded code, fired over
// and over, building the same component forever. That demonstrates the
// primitive and nothing else, and the sim was nonetheless classified as
// self-replicating on the strength of von Neumann's architecture rather than on
// anything it did — the exact overclaim this bench exists to refuse, sitting in
// its own flagship entry.
//
// So it has a tape. The description is a row of REAL cells in the lattice, not
// an array on the side: north-pointing ordinary transmission is a 1, north-
// pointing special transmission is a 0. They point across the tape rather than
// along it, so no cell on the tape is an input to its neighbour and the row
// does not evolve on its own, however long it runs. You can see it in the grid.
//
// It is not sealed, though, and saying otherwise would be the same overclaim in
// miniature. The tape is in the same lattice as everything else, so anything
// that reaches it can wreck it: set the last program's code to "ordinary N" and
// the arm's own wire climbs the three rows of clearance and excites a tape cell
// — measured on a 70x40 grid, "tape intact" drops at step 34. No placement of
// the row escapes that, because the code picks the direction. What the sim does
// instead is watch: "tape intact" re-reads the row out of the lattice and
// compares it with what was written, so corruption shows up as a metric rather
// than as a program that quietly got shorter. The brush is the one corruption
// that IS prevented, by poke() refusing the tape row outright.
//
// A read head walks that row and fires the bits down the arm. Codes are 4 or 5
// bits and self-delimiting: the sensitized tree tells you when it has reached a
// leaf, so the head knows an instruction has ended without being told a length.
// It is the same tree the lattice walks, so the head cannot disagree with the
// automaton about where an instruction stops.
//
// WHAT THIS IS NOT, stated plainly because the whole point was to stop claiming
// otherwise: there is no universal constructor here and no self-replication.
// The read head is a host-side pointer, not a machine built out of cells — the
// real thing needs a read head, a copier and a tape describing all three, which
// is 145,315 tape cells and some 63 billion timesteps in the Nobili-Pesavento
// implementation. What this DOES demonstrate is the step the arm alone could
// not: the machine builds what it is TOLD to build. Change the tape and a
// different structure appears, with nothing else touched.

#pragma once
#include "../sim.hpp"
#include "../parallel.hpp"
#include <array>
#include <cstdio>
#include <deque>

namespace bench {

namespace jvn {

// state encoding
inline constexpr std::uint8_t U = 0;                                    // quiescent
inline constexpr std::uint8_t ots(int d, int e) { return std::uint8_t(1 + d * 2 + e); }   // 1..8
inline constexpr std::uint8_t sts(int d, int e) { return std::uint8_t(9 + d * 2 + e); }   // 9..16
inline constexpr std::uint8_t con(int a, int b) { return std::uint8_t(17 + a * 2 + b); }  // 17..20
inline constexpr std::uint8_t S_ = 21, S0 = 22, S1 = 23, S00 = 24,
                              S01 = 25, S10 = 26, S11 = 27, S000 = 28;

inline constexpr bool is_o(std::uint8_t s) { return s >= 1  && s <= 8;  }
inline constexpr bool is_t(std::uint8_t s) { return s >= 9  && s <= 16; }
inline constexpr bool is_c(std::uint8_t s) { return s >= 17 && s <= 20; }
inline constexpr bool is_s(std::uint8_t s) { return s >= 21; }
inline constexpr int  o_dir(std::uint8_t s) { return (s - 1) >> 1; }
inline constexpr int  o_exc(std::uint8_t s) { return (s - 1) & 1; }
inline constexpr int  t_dir(std::uint8_t s) { return (s - 9) >> 1; }
inline constexpr int  t_exc(std::uint8_t s) { return (s - 9) & 1; }
inline constexpr int  c_a  (std::uint8_t s) { return (s - 17) >> 1; }
inline constexpr int  c_b  (std::uint8_t s) { return (s - 17) & 1; }
inline constexpr int  dir_of(std::uint8_t s) { return is_o(s) ? o_dir(s) : is_t(s) ? t_dir(s) : -1; }

// 0=E 1=N 2=W 3=S
inline constexpr int DX[4] = { 1, 0, -1, 0 };
inline constexpr int DY[4] = { 0, -1, 0, 1 };

// ── tape symbols ────────────────────────────────────────────────────────────
//
// A 1 is ordinary transmission pointing north; a 0 is special transmission
// pointing north. Both point ACROSS a tape written east-west, so no tape cell
// is an input to the cell beside it and the row does not evolve on its own.
//
// That is a claim about the row in isolation and nothing more. A cell BELOW a
// tape cell and pointing north is an input to it, so a signal arriving from
// the construction area can excite the row — which is what the "tape intact"
// metric watches for. See the header.
//
// They also render as different palette categories, so the tape is legible in
// the grid rather than being a row of identical squares.
inline std::uint8_t tape_cell(int bit) { return bit ? ots(1, 0) : sts(1, 0); }
inline bool is_tape_cell(std::uint8_t s) { return s == ots(1, 0) || s == sts(1, 0); }
inline int  tape_bit(std::uint8_t s) { return s == ots(1, 0) ? 1 : 0; }

// the sensitized binary tree: [on input 0, on input 1]
inline std::uint8_t tree(std::uint8_t s, int bit) {
    switch (s) {
        case S_:   return bit ? S1        : S0;
        case S0:   return bit ? S01       : S00;
        case S1:   return bit ? S11       : S10;
        case S00:  return bit ? ots(2, 0) : S000;      // 1001 -> ordinary W
        case S01:  return bit ? sts(0, 0) : ots(3, 0); // 1011 -> special E / 1010 -> ordinary S
        case S10:  return bit ? sts(2, 0) : sts(1, 0); // 1101 -> special W / 1100 -> special N
        case S11:  return bit ? con(0, 0) : sts(3, 0); // 1111 -> confluent / 1110 -> special S
        case S000: return bit ? ots(1, 0) : ots(0, 0); // 10001 -> ord N / 10000 -> ord E
        default:   return s;
    }
}

} // namespace jvn

class VonNeumann final : public Sim {
public:
    explicit VonNeumann(int w = 128, int h = 72)
        : w_(w), h_(h), baseW_(w), baseH_(h), view_(w, h) {
        about_ = Provenance{
            "Von Neumann's 29-state automaton", "c. 1952",
            "John von Neumann, on Stanislaw Ulam's suggestion to drop the robot for a lattice",
            "von Neumann, J. Theory of Self-Reproducing Automata, ed. A. W. Burks, "
            "University of Illinois Press, 1966",
            Replication::No,
            "No — and this verdict has now been wrong in both directions, which is worth "
            "leaving on the record. Von Neumann's ARCHITECTURE replicates, and was designed to "
            "from first principles: a universal constructor plus a universal computer, reading "
            "a tape that describes both. It was long classified here as Yes on the strength of "
            "that, which is an argument from the literature about a machine this file does not "
            "contain. Correcting it to Disputed was still wrong: every other Disputed entry on "
            "the bench is a case where a genome IS copied with variation and the engine merely "
            "does the copying, and nothing here copies anything at all. What runs is a "
            "tape-driven constructor - a description in the lattice and an arm that builds what "
            "it says - with no copier, no universality, and a read head that is a host-side "
            "pointer rather than a machine made of cells. The full thing is 145,315 tape cells "
            "and some 63 billion timesteps in the 1995 Nobili-Pesavento implementation, which "
            "is precisely why Conway went looking for something smaller.",
            "A tape and an arm. The description is a row of real cells - ordinary transmission "
            "pointing north is a 1, special transmission pointing north is a 0 - and a read "
            "head fires those bits down the arm. A quiescent cell struck by the pulse train "
            "walks the sensitized tree, one level per tick, then crystallises into whatever the "
            "code named. Change the tape and a different structure gets built, which is the "
            "step a fixed arm could not show: construction is not a special mode, it is what "
            "ordinary signalling does when it runs out of wire."
        };
        pal_ = {
            {{  8, 11, 14}, "quiescent"},
            {{242,193, 78}, "ordinary transmission"},
            {{255,255,255}, "ordinary, excited"},
            {{217, 83, 79}, "special transmission"},
            {{255,214,214}, "special, excited"},
            {{155,122,230}, "confluent"},
            {{238,228,255}, "confluent, outputting"},
            {{ 90,209,196}, "sensitized (mid-construction)"},
        };
        knobs_ = {
            {"program", "what the tape says", 0.f, 4.f, 0.f, 1.f,
             {"wire, then a confluent", "wire, then a special", "a longer wire",
              "wire, confluent, wire", "one code, repeated"}, true,
             "The description written onto the tape, as a row of real cells in the lattice. "
             "Nothing else changes when you change this — the same read head fires the same "
             "way and the same rule runs — and a different structure gets built. That is the "
             "whole difference between an arm that builds one thing and a machine that builds "
             "what it is told.  ·  Only as much tape as fits on the grid is written: on the "
             "56-wide grid the bench ships, \"a longer wire\" is 60 bits and 51 of them land. "
             "One step of the size knob is enough to land all 60."},
            {"code", "code for the repeated program", 0.f, 5.f, 0.f, 1.f,
             {"10000 ordinary E", "10001 ordinary N", "1001 ordinary W",
              "1010 ordinary S", "1011 special E", "1111 confluent"}, true,
             "Which of von Neumann's codes the last program repeats. Every one of them is "
             "asserted in the self-test against the cell it must produce, so this selects "
             "between verified behaviours rather than between guesses.  ·  Only the last "
             "program reads it; the others spell out their own instructions on the tape.",
             false, false, "program", 4.f}};

        // ── world size ─────────────────────────────────────────────────────
        //
        // Doublings of the size this sim was CONSTRUCTED at, not a fixed list
        // of dimensions. Position 0 is therefore always exactly the grid the
        // caller asked for, so the shipped 56x32 keeps its published behaviour
        // and the knob still reads honestly for the 40x20 and 70x40 grids the
        // self-test builds — a hardcoded list would have mislabelled both.
        //
        // WIDTH is the dimension that buys anything here, and it buys two
        // things. The tape is one row of real cells that stops three columns
        // short of the edge, so at the shipped 56 wide the longest program
        // loses 9 of its 60 bits; one doubling and the whole tape lands. Width
        // is also the runway: construction resets when anything reaches the
        // last column, and how long that takes is linear in width — measured,
        // 434 steps at 56 wide then 934, 1942, 3958, 7990 and 16,054, which is
        // a clean doubling per doubling and about nine steps a column. Height
        // comes along to keep the picture the shape it was; it is empty
        // lattice, and it is most of the cost.
        {
            std::vector<std::string> sizes;
            for (int i = 0; i < 16; ++i) {
                const long long cw = (long long)baseW_ << i, ch = (long long)baseH_ << i;
                if (i > 0 && cw * ch > kMaxCells) break;
                sizes.push_back(std::to_string(cw) + "x" + std::to_string(ch));
            }
            // One position is not a control. A sim built so large that it
            // cannot double simply does not offer the knob, rather than
            // offering a slider that cannot move.
            if (sizes.size() > 1) {
                sizeSteps_ = int(sizes.size()) - 1;
                knobs_.emplace_back(
                    "size", "world size", 0.f, float(sizeSteps_), 0.f, 1.f, sizes, true,
                    "Cells across by cells down, doubling both. Position 0 is the grid this "
                    "sim was built with, so leaving it alone changes nothing.  ·  WIDTH is "
                    "what it buys. The tape stops three columns short of the edge, so at the "
                    "shipped 56 wide the longest program loses 9 of its 60 bits and one "
                    "doubling lands all of them; and the wire runs proportionally further "
                    "before it reaches the last column and construction restarts — measured "
                    "under program 2, 434 steps at 56 wide against 16,054 at 1792; the "
                    "default program restarts on its own schedule and does not scale that "
                    "way.  ·  Cost: the top size is about 3 ms a step on one thread and 2 "
                    "threaded, measured after 4,000 steps of growth rather than on the empty "
                    "grid a fresh reset leaves behind.");
            }
        }
        reset();
    }

    const Provenance&          about()   const override { return about_; }
    const std::vector<Swatch>& palette() const override { return pal_;   }
    const Field&               field()   const override { return view_;  }
    std::uint64_t              generation() const override { return gen_; }

    std::vector<Metric> metrics() const override {
        // "tape intact" is read back out of the lattice and compared with what
        // was written there. A description that the rule has quietly eaten is
        // the one failure that would make everything else here meaningless, and
        // it is cheap to watch.
        const std::vector<int> live = read_tape();
        const bool intact = (live == tape_);
        return {
            Metric{ "tape length (bits)", double(tape_.size()), 0.0, Metric::Neither },
            Metric{ "instructions read", double(built_), 0.0, Metric::Neither },
            Metric{ "tape intact", intact ? 1.0 : 0.0, 1.0, Metric::Higher },
            Metric{ "times round the tape", double(passes_), 0.0, Metric::Neither },
        };
    }

    [[nodiscard]] std::string subtitle() const override {
        char b[192];
        std::snprintf(b, sizeof b,
                      "tape %zu bits  ·  head at %zu  ·  %d instructions built  ·  %d passes",
                      tape_.size(), head_, built_, passes_);
        return b;
    }

    void reset() override {
        raw_.assign(std::size_t(w_) * h_, jvn::U);
        view_ = Field(w_, h_);
        gen_ = 0; queue_.clear(); bits_.clear(); bit_at_ = 0;
        passes_ = 0;      // was surviving reset() while every other counter zeroed
        arm_y_ = h_ / 2;
        for (int x = 2; x <= 6; ++x) raw_[idx(x, arm_y_)] = jvn::ots(0, 0);
        writeTape(program());
        head_ = 0; built_ = 0;
        republish();
    }

    // ── the tape ───────────────────────────────────────────────────────────
    //
    // Written into the lattice as real cells, three rows above the arm. Every
    // tape cell points north, across the tape rather than along it, so no cell
    // on it is an input to its neighbour: the row does not evolve on its own.
    //
    // That is a claim about the row in isolation and nothing more. Three rows
    // of clearance is not protection from a program that builds NORTHWARD — the
    // arm's wire climbs into the row within tens of steps, and the "tape intact"
    // metric is what reports it. See the header.
    void writeTape(const std::vector<int>& bits) {
        tape_.clear();
        tapeY_ = std::max(1, arm_y_ - 3);
        // Stop three columns short of the edge, so a long tape cannot reach
        // the region the arm is meant to build into. A tape too long for the
        // grid is therefore cut short — 60 bits down to 51 for the longest
        // program at the shipped 56 wide, and all 60 from 112 wide up, which is
        // the first thing the size knob buys — and the "tape length (bits)"
        // metric reports what actually landed rather than what was asked for.
        // Cutting mid-code is safe rather than lucky: every proper prefix of a code is
        // an interior node of the sensitized tree, so the head runs off the end
        // still walking and discards the partial instruction instead of
        // decoding some other one.
        for (std::size_t i = 0; i < bits.size(); ++i) {
            const int x = kTapeX + int(i);
            if (x >= w_ - 3) break;
            raw_[idx(x, tapeY_)] = jvn::tape_cell(bits[i]);
            tape_.push_back(bits[i]);
        }
    }

    // Read the tape back OUT of the lattice, not out of the vector that wrote
    // it. If the two ever disagree the tape has been corrupted by the rule, and
    // that is worth knowing rather than papering over.
    [[nodiscard]] std::vector<int> read_tape() const {
        std::vector<int> out;
        for (int x = kTapeX; x < w_ - 1; ++x) {
            const std::uint8_t c = raw_[idx(x, tapeY_)];
            if (!jvn::is_tape_cell(c)) break;
            out.push_back(jvn::tape_bit(c));
        }
        return out;
    }

    // The programs. Each is a sequence of von Neumann's real codes, and the
    // tape is their concatenation — the codes are self-delimiting, so the head
    // needs no separators and no lengths.
    [[nodiscard]] std::vector<int> program() const {
        static const std::vector<int> OE{1,0,0,0,0}, ON{1,0,0,0,1}, OW{1,0,0,1},
                                      OS{1,0,1,0}, SE{1,0,1,1}, CO{1,1,1,1};
        // By key, not by index. These read knobs_[0] and knobs_[1] until the
        // size knob was added, at which point the whole tape would have been
        // one insertion away from silently selecting the wrong program.
        const int which = knob_index("program");
        std::vector<int> t;
        auto add = [&](const std::vector<int>& c) { t.insert(t.end(), c.begin(), c.end()); };
        switch (which) {
            // Braced. Unbraced these read as though the add() after the loop
            // were inside it, which is the one place a reader would take the
            // tape's contents on trust — and the tape's contents are the whole
            // claim here.
            case 0: { for (int i = 0; i < 4; ++i)  add(OE); add(CO); break; }
            case 1: { for (int i = 0; i < 4; ++i)  add(OE); add(SE); break; }
            case 2: { for (int i = 0; i < 12; ++i) add(OE);          break; }
            case 3: {
                for (int i = 0; i < 3; ++i) add(OE);
                add(CO);
                for (int i = 0; i < 3; ++i) add(OE);
                break;
            }
            default: {
                static const std::vector<int>* codes[6] = { &OE, &ON, &OW, &OS, &SE, &CO };
                const int c = knob_index("code");
                for (int i = 0; i < 6; ++i) add(*codes[(c >= 0 && c < 6) ? c : 0]);
                break;
            }
        }
        return t;
    }

    // Read ONE instruction off the tape and queue it.
    //
    // Codes are 4 or 5 bits and self-delimiting: the head walks the same
    // sensitized tree the lattice walks, and stops when it reaches a leaf. That
    // is not a convenience — it is why von Neumann's tape needs no separators
    // and no lengths, and using the same tree() the rule uses means the head
    // cannot disagree with the automaton about where an instruction ends.
    void readInstruction() {
        const std::vector<int> tape = read_tape();
        if (tape.empty()) return;
        if (head_ >= tape.size()) { head_ = 0; ++passes_; }

        std::vector<int> code;
        // The leading bit STRIKES the quiescent cell and is not a tree input.
        // Every code begins with a 1 for exactly that reason: U -> S_ happens on
        // the pulse arriving, and only the bits after it walk the tree. Feeding
        // that first bit to tree() decodes every instruction one bit out of
        // phase — the tape said "wire, wire, wire, confluent" and the machine
        // built "special north, ordinary west, ordinary east, ...". Sharing
        // tree() with the rule was not enough to keep the head honest, because
        // the two also have to agree on where an instruction STARTS.
        if (tape[head_] != 1) { ++head_; return; }
        code.push_back(tape[head_++]);

        std::uint8_t node = jvn::S_;                  // struck: now walk the tree
        while (head_ < tape.size() && code.size() < 8) {
            const int bit = tape[head_++];
            code.push_back(bit);
            node = jvn::tree(node, bit);
            if (!jvn::is_s(node)) break;              // crystallised: instruction over
        }
        if (jvn::is_s(node)) return;                  // ran off the end mid-code
        ++built_;
        fire(code);
    }

    // Queue one of the nine real construction codes. readInstruction() is the
    // only caller, and deliberately so: what gets built comes off the tape, with
    // no second path in from a knob.
    void fire(const std::vector<int>& code) {
        if (queue_.size() < 12) {
            std::vector<int> c = code;
            c.push_back(0); c.push_back(0);   // let the tail clear the wire
            queue_.push_back(std::move(c));
        }
    }

    std::vector<Knob>& knobs() override { return knobs_; }
    void on_knob(const std::string& k, float v) override {
        for (auto& kn : knobs_) if (kn.key == k) kn.value = v;
        if (k == "size") {
            // Set the dimensions, then reset — reset() is what actually
            // rebuilds raw_ and view_, and rule() resizes nxt_raw_ behind it.
            // Assigning w_/h_ alone would leave every buffer at the old size
            // and the knob would be a no-op that looked like it worked, which
            // is exactly the shape this bug takes: field().w still reports the
            // old width and nothing on screen changes.
            const int i = std::clamp(int(v + 0.5f), 0, sizeSteps_);
            const int nw = baseW_ << i, nh = baseH_ << i;
            if (nw != w_ || nh != h_) { w_ = nw; h_ = nh; reset(); }
        }
        if (k == "code") reset();     // the arm rebuilds from scratch with the new code
    }

    void step() override {
        if (bit_at_ >= bits_.size() && !queue_.empty()) {
            bits_ = queue_.front(); queue_.pop_front(); bit_at_ = 0;
        }
        // Nothing queued: read the next instruction off the tape. When the tape
        // runs out the head returns to its start, so the bench has something to
        // watch — and the counter says how many times it has been round, which
        // is the honest way to show a loop rather than pretending it is one
        // long program.
        if (bit_at_ >= bits_.size() && queue_.empty()) readInstruction();

        rule();

        if (bit_at_ < bits_.size()) {
            raw_[idx(2, arm_y_)] = jvn::ots(0, bits_[bit_at_]);
            ++bit_at_;
        }
        ++gen_;

        // Skipping the tape row is belt-and-braces, and worth saying so rather
        // than letting it read as the load-bearing guard.
        //
        // The tape is written along the same axis the arm builds along, so a
        // long enough tape could reach the column this scans and trip the
        // "construction has run off the end" detector — reset() every step, and
        // a sim that froze with zero instructions built, looking like a hang
        // rather than a bug. What actually prevents that now is writeTape's
        // `x >= w_ - 3` stop: measured at widths 40, 56, 70 and 100, the
        // longest program's tape ends two or more columns short of the scanned
        // one every time — and the size knob does not weaken that, because it
        // only ever makes the grid wider while the longest tape stays 60 bits:
        // at 56 wide the stop leaves two columns of clearance, and at every
        // width above it the tape ends at column 61 with the scan at w-2.
        // Removing this skip changes nothing while that stop is
        // there. It is kept because the two guards protect different things —
        // the stop bounds where a tape is written, this bounds what the
        // detector is allowed to notice.
        bool at_edge = false;
        for (int y = 0; y < h_; ++y) {
            if (y == tapeY_) continue;
            if (raw_[idx(w_ - 2, y)] != jvn::U) at_edge = true;
        }
        if (at_edge) reset();
        republish();
    }

    // Test hooks
    // Draw a short run of ordinary transmission wire pointing east. The
    // rendered field is a derived 8-category view of 29 real states, so the
    // brush cannot write to it — this writes to the state that actually steps.
    //
    // The tape row is refused. A stroke there does not merely spoil a metric:
    // read_tape() stops at the first cell that is not a tape symbol, so east
    // wire dropped on the row truncates the program the head is executing,
    // mid-run. The "tape intact" metric catches that, since it re-reads the row
    // out of the lattice and compares; "tape length (bits)" does not move,
    // because it reports the length as written.
    //
    // Every other row is fair game, except the top and bottom: the inner guard
    // drops the whole stroke there while poke() still reports success.
    bool poke(float nx, float ny) override {
        const int cx = int(nx * w_), cy = int(ny * h_);
        if (cy == tapeY_) return false;
        for (int dx = -3; dx <= 3; ++dx) {
            const int x = cx + dx;
            if (x < 1 || x >= w_ - 1 || cy < 1 || cy >= h_ - 1) continue;
            raw_[idx(x, cy)] = jvn::ots(0, 0);          // ordinary, east, unexcited
        }
        return true;
    }

    [[nodiscard]] std::uint8_t raw_at(int x, int y) const { return raw_[idx(x, y)]; }
    void clear_raw() { raw_.assign(std::size_t(w_) * h_, jvn::U); }
    void set_raw(int x, int y, std::uint8_t v) { raw_[idx(x, y)] = v; }
    void rule_only() { rule(); ++gen_; republish(); }
    [[nodiscard]] int arm_row() const { return arm_y_; }
    [[nodiscard]] int tape_row() const { return tapeY_; }
    [[nodiscard]] static int tape_x() { return kTapeX; }
    [[nodiscard]] int instructions_read() const { return built_; }

private:
    [[nodiscard]] std::size_t idx(int x, int y) const { return std::size_t(y) * w_ + x; }

    // Which position a choice knob is on, by key. See program().
    [[nodiscard]] int knob_index(const char* key) const {
        for (const auto& k : knobs_) if (k.key == key) return int(k.value + 0.5f);
        return 0;
    }

    void rule() {
        const int w = w_, h = h_;
        // resize, not assign. Every cell of nxt_raw_ is written unconditionally
        // below — the only `continue` is inside the neighbour loop — so copying
        // raw_ in first was a whole extra pass over the lattice whose result was
        // overwritten. At 1792x1024 that copy is 1.8 MB a step.
        if (nxt_raw_.size() != raw_.size()) nxt_raw_.resize(raw_.size());
        // Row-parallel above a measured threshold; see kParallelCells.
        //
        // Rows are independent by construction and nothing here is shared or
        // mutable: every read comes from raw_, every write goes to this row's
        // own slice of nxt_raw_, and this sim has no Rng at all — there is no
        // state a split could reorder. Verified rather than argued: the same
        // number of steps run at one worker and at sixteen give fields that
        // compare equal cell for cell.
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        parallel_for(std::size_t(h), [&](std::size_t row) {
            const int y = int(row);
            for (int x = 0; x < w; ++x) {
                const std::uint8_t s = raw_[idx(x, y)];
                bool ord_exc = false, spc_exc = false, conf_out = false;
                bool any_ord = false, all_ord = true;
                const int md = jvn::dir_of(s);
                for (int k = 0; k < 4; ++k) {
                    const int nx = x + jvn::DX[k], ny = y + jvn::DY[k];
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    const std::uint8_t n = raw_[idx(nx, ny)];
                    if (n == jvn::U) continue;
                    const bool points_at_me =
                        (jvn::is_o(n) || jvn::is_t(n)) && jvn::dir_of(n) == ((k + 2) & 3);
                    if (jvn::is_o(n) && points_at_me) {
                        any_ord = true;
                        if (jvn::o_exc(n)) ord_exc = true; else all_ord = false;
                    }
                    if (jvn::is_t(n) && points_at_me && jvn::t_exc(n)) spc_exc = true;
                    // a confluent outputs to transmission cells that do not point back at it
                    if (jvn::is_c(n) && jvn::c_a(n) == 1 &&
                        (jvn::is_o(s) || jvn::is_t(s)) && md != k) conf_out = true;
                }
                std::uint8_t ns = s;
                if (s == jvn::U)          ns = (ord_exc || spc_exc) ? jvn::S_ : jvn::U;
                else if (jvn::is_s(s))    ns = jvn::tree(s, (ord_exc || spc_exc) ? 1 : 0);
                else if (jvn::is_o(s))    ns = spc_exc ? jvn::U
                                             : jvn::ots(jvn::o_dir(s), (ord_exc || conf_out) ? 1 : 0);
                else if (jvn::is_t(s))    ns = ord_exc ? jvn::U
                                             : jvn::sts(jvn::t_dir(s), (spc_exc || conf_out) ? 1 : 0);
                else if (jvn::is_c(s))    ns = spc_exc ? jvn::U
                                             : jvn::con(jvn::c_b(s), (any_ord && all_ord) ? 1 : 0);
                nxt_raw_[idx(x, y)] = ns;
            }
        }, cells >= kParallelCells ? 0u : 1u);
        raw_.swap(nxt_raw_);
    }

    // 29 states collapse to 8 render categories, and direction is DISCARDED
    // here: all four ordinary transmission directions publish as the same
    // index. Nothing downstream can recover it — the renderer is handed this
    // field and the palette and nothing else — which is why the self-test reads
    // raw_at() rather than the published field when it needs to know which way
    // a cell points.
    //
    // Row-parallel on the same threshold as the sweep. It is the cheaper of the
    // two passes, but it is a second full pass over the lattice and at the top
    // size that is milliseconds; the same independence argument applies, since
    // each row reads its own slice of raw_ and writes its own slice of view_.
    void republish() {
        const int w = w_, h = h_;
        const std::size_t cells = std::size_t(w) * std::size_t(h);
        parallel_for(std::size_t(h), [&](std::size_t row) {
            const std::size_t base = row * std::size_t(w);
            for (int x = 0; x < w; ++x) {
                const std::size_t i = base + std::size_t(x);
                const std::uint8_t s = raw_[i];
                std::uint8_t c;
                if (s == jvn::U)        c = 0;
                else if (jvn::is_o(s))  c = jvn::o_exc(s) ? 2 : 1;
                else if (jvn::is_t(s))  c = jvn::t_exc(s) ? 4 : 3;
                else if (jvn::is_c(s))  c = jvn::c_a(s) == 1 ? 6 : 5;
                else                    c = 7;
                view_.cells[i] = c;
            }
        }, cells >= kParallelCells ? 0u : 1u);
    }

    Provenance                about_;
    std::vector<Swatch>       pal_;
    std::vector<Knob>         knobs_;
    // The real state is raw_ (29 states, one byte each) double-buffered through
    // nxt_raw_; view_ is the 8-category field the renderer reads. Only the size
    // is shared between them, so it is kept as two ints rather than as Field
    // members whose cell buffers nothing would ever touch.
    int                       w_, h_;
    // The size this sim was constructed at, which the size knob doubles from.
    int                       baseW_ = 0, baseH_ = 0;
    int                       sizeSteps_ = 0;      // last position of the size knob

    // The largest world the size knob will offer, in cells.
    //
    // Not a round number chosen for looks. Cost is dead linear at about 158,000
    // cells per millisecond on one thread, so this is roughly 16 ms a step
    // serial and a third of that threaded — still something you can run rather
    // than something you can only look at. The shipped 56x32 stops one doubling
    // below it, at 1792x1024, which measured 11.7 ms serial and 5.6 threaded.
    static constexpr long long kMaxCells = 2500000;

    // Above this many cells the two sweeps are worth threading.
    //
    // Higher than the 250,000 the lattice sims use, and measured rather than
    // copied: a parallel_for costs about 0.85 ms here before it does any work
    // (16 workers, ~0.06 ms per thread launch), and a step pays that twice
    // because the rule sweep and the republish pass are separate loops. At
    // 158,000 cells/ms that fixed cost is not repaid until roughly 290,000
    // cells, and the measured crossover agrees: 448x256 is 114,688 cells and
    // threading it is a wash or worse (0.96x), 896x512 is 458,752 and gains
    // 1.4-1.8x. Below this the split is pure overhead and the sweep stays
    // serial — which is also why the shipped 56x32 is untouched by all of it.
    static constexpr std::size_t kParallelCells = 300000;

    Field                     view_;
    std::vector<std::uint8_t> raw_, nxt_raw_;
    std::deque<std::vector<int>> queue_;
    std::vector<int>          bits_;
    std::size_t               bit_at_ = 0;
    int                       arm_y_  = 0;
    static constexpr int      kTapeX  = 2;
    int                       tapeY_  = 0;
    std::vector<int>          tape_;
    std::size_t               head_   = 0;
    int                       built_  = 0, passes_ = 0;
    std::uint64_t             gen_    = 0;
};

inline SimPtr make_von_neumann(int w = 128, int h = 72) {
    return std::make_unique<VonNeumann>(w, h);
}

} // namespace bench
