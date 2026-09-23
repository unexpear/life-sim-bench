// history.hpp — snapshots, rewind and metric traces.
//
// The reason this exists: in a cellular automaton the interesting event is
// always three generations behind you by the time you notice it. Without a
// timeline you reset and hope it happens again, which for anything stochastic
// means it does not. With one you scrub back and look.
//
// Two separate stores, because they have very different costs:
//
//   Snapshots — whole Field copies at a coarse interval. Memory-bounded by
//               total bytes, not by count, so a 190x190 colony and a 512x512
//               soup both behave sensibly.
//   Traces    — one double per metric per sample. Cheap, so kept dense.

#pragma once
#include "field.hpp"
#include "sim.hpp"
#include <chrono>
#include <cmath>
#include <deque>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace bench {

// Has it stopped improving?
//
// Split out of the workbench's frame loop so it can be tested at all. "How
// many generations until it plateaus" is the question every measurement in
// this project needed answered, and counting it off a plot by eye is exactly
// the work the bench should be doing.
//
// Fed the best-so-far, which is monotone in the metric's own direction, so
// "improved" is simply "changed". That is the whole trick: it works for
// lower-is-better series without knowing which they are.
class Plateau {
public:
    void reset() { best_ = kNone; since_ = 0; }
    // True on the sample where it has been stale for `limit` epochs — once,
    // not on every sample after. The caller stops training on that edge.
    bool observe(double best, int limit) {
        if (best_ == kNone || std::fabs(best - best_) > 1e-9) {
            best_ = best; since_ = 0; return false;
        }
        return ++since_ == limit;
    }
    [[nodiscard]] int  since() const { return since_; }
    [[nodiscard]] double best() const { return best_; }

private:
    static constexpr double kNone = -1e300;
    double best_  = kNone;
    int    since_ = 0;
};

class History {
public:
    // Which clock a series is sampled against.
    //
    // This distinction is the whole point of the epoch work. A frame sample
    // answers "what is it doing right now"; an epoch sample answers "is it
    // learning". Plotting a learning curve against frames is a category error:
    // one NEAT generation is eighty genomes playing to death, thousands of
    // frames, and a per-frame curve is mostly the inside of a single generation
    // — the shape you see is the last genome's run, not the population's
    // progress. Sampling once per completed epoch is the only reading of
    // "best fitness" that means anything.
    enum class Axis { Frame, Epoch };

    // budget_bytes caps total snapshot memory; every_n is the sampling interval.
    void configure(std::size_t budget_bytes, int every_n) {
        budget_ = budget_bytes;
        every_  = every_n < 1 ? 1 : every_n;
    }

    // Give memory back under pressure. memoryTight() used to gate only the
    // GROWTH of the ring, so a timeline sized when 8 GB was free stayed 300 MB
    // large after the machine filled up. Stopping recording is not the same as
    // releasing what is already held.
    void shrink_to(std::size_t budget) {
        while (bytes_ > budget && snaps_.size() > 1) {
            const std::size_t sz = snaps_.front().field.cells.size();
            bytes_ = (bytes_ > sz) ? bytes_ - sz : 0;
            snaps_.pop_front();
        }
        budget_ = std::min(budget_, budget);   // and stop re-growing to the old size
    }

    // Best-so-far for a named series. A learning curve's raw trace is noisy
    // enough that "is it improving" is genuinely hard to read off it; the
    // running maximum is the line people actually want and it costs nothing.
    [[nodiscard]] double best_of(const std::string& name, Axis ax = Axis::Frame) const {
        const Trace* t = trace(name, ax);
        if (!t || t->values.empty()) return 0.0;
        double b = t->values.front();
        if (t->better == Metric::Neither) return t->values.back();
        for (double v : t->values)
            b = (t->better == Metric::Higher) ? std::max(b, v) : std::min(b, v);
        return b;
    }
    [[nodiscard]] double mean_of(const std::string& name, std::size_t lastN,
                                 Axis ax = Axis::Frame) const {
        const Trace* t = trace(name, ax);
        if (!t || t->values.empty()) return 0.0;
        const auto& v = t->values;
        const std::size_t n = std::min(lastN, v.size());
        double s = 0;
        for (std::size_t i = v.size() - n; i < v.size(); ++i) s += v[i];
        return s / double(n);
    }

    // Write every recorded series to CSV. A run that cannot leave the window
    // cannot be compared against another run, and comparing runs is most of
    // what measuring a learning agent consists of.
    bool write_csv(const std::string& path, Axis ax = Axis::Frame) const {
        std::ofstream out(path);
        if (!out) return false;
        const std::vector<std::string>& names = (ax == Axis::Epoch) ? eorder_ : order_;
        // The first column names the clock, because a column of numbers with no
        // stated units is how two runs get compared that were never comparable.
        out << ((ax == Axis::Epoch) ? "epoch" : "sample");
        for (const auto& n : names) out << "," << n;
        out << "\n";
        std::size_t longest = 0;
        for (const auto& n : names)
            if (const Trace* t = trace(n, ax)) longest = std::max(longest, t->values.size());
        for (std::size_t i = 0; i < longest; ++i) {
            // An epoch row carries the epoch number it was taken at, not its
            // position in the buffer: several epochs can complete inside one
            // frame, and renumbering them 0,1,2 would close a real gap on the
            // x-axis without saying so.
            out << ((ax == Axis::Epoch) ? epoch_index(i) : int(i));
            for (const auto& n : names) {
                out << ",";
                if (const Trace* t = trace(n, ax))
                    if (i < t->values.size()) out << t->values[i];
            }
            out << "\n";
        }
        return true;
    }

    void clear() {
        snaps_.clear(); bytes_ = 0;
        traces_.clear(); order_.clear(); samples_ = 0;
        etraces_.clear(); eorder_.clear(); epochAt_.clear();
        lastEpoch_ = 0;
        lastSnapGen_ = 0; haveClock_ = false;
    }
    // Slowest cadence at which snapshots may be taken, in milliseconds.
    void set_min_interval(int ms) { minIntervalMs_ = ms < 0 ? 0 : ms; }

    // Large runs can retain measurements without copying their entire field.
    void set_record_snapshots(bool enabled) {
        recordSnapshots_ = enabled;
        lastSnapGen_ = 0;
        if (!enabled) { snaps_.clear(); bytes_ = 0; haveClock_ = false; }
    }
    [[nodiscard]] bool recording_snapshots() const { return recordSnapshots_; }

    // Call once per FRAME, not once per step.
    //
    // This distinction is the whole reason this comment exists. The snapshot
    // interval used to be measured purely in generations, which is fine at 30
    // steps/second and catastrophic at 2000 steps/frame: generations advance
    // arbitrarily fast, so the snapshot RATE was unbounded. Copying a 250x250
    // field 500 times a frame is roughly a gigabyte per second of allocate-and-
    // immediately-evict. The retained size stayed inside its budget the whole
    // time; the churn is what killed it.
    //
    // So snapshots are now bounded three ways: by generation interval, by
    // wall-clock rate, and by count. Any one of them alone is insufficient.
    void observe(const Sim& sim) {
        const std::uint64_t g = sim.generation();

        // metric traces: cheap, but cap how many distinct series exist — a
        // plugin is free to invent a new metric name every step, and nothing
        // else here would stop it.
        for (const auto& m : sim.metrics()) {
            auto it = traces_.find(m.name);
            if (it == traces_.end()) {
                if (order_.size() >= kMaxTraces) continue;
                order_.push_back(m.name);
                it = traces_.emplace(m.name, Trace{}).first;
                it->second.max_hint      = m.max;
                it->second.better        = m.better;
            }
            auto& t = it->second;
            t.values.push_back(m.value);
            if (t.values.size() > kTraceCap) t.values.pop_front();
        }
        ++samples_;

        // epoch traces: recorded when the sim's own epoch counter moves, so a
        // generation that completes while free-running is captured just the
        // same as one produced by clicking "+1 generation". Detecting it here
        // rather than at each call site is deliberate — there are five places
        // that advance a sim, and an epoch missed by one of them is a hole in
        // the curve that looks exactly like a plateau.
        // lastEpoch_ starts at 0 rather than being primed from the first
        // observation on purpose. Priming it swallowed the FIRST completed
        // epoch — the axis read "generation 2-6" after six generations — and a
        // learning curve missing its first point is missing the steepest part
        // of itself.
        if (sim.epoch_name()) {
            const int e = sim.epoch_count();
            if (e != lastEpoch_) { lastEpoch_ = e; recordEpoch(sim, e); }
        }

        // snapshots: coarse, budgeted, and rate-limited
        if (!recordSnapshots_) return;
        if (g - lastSnapGen_ < std::uint64_t(every_) && lastSnapGen_ != 0) return;
        const auto now = std::chrono::steady_clock::now();
        if (haveClock_) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                now - lastSnapAt_).count();
            if (ms < minIntervalMs_) return;
        }
        lastSnapAt_ = now; haveClock_ = true; lastSnapGen_ = g;

        const Field& f = sim.field();
        snaps_.push_back(Snap{ g, f });
        bytes_ += f.cells.size();

        while ((bytes_ > budget_ || snaps_.size() > kMaxSnaps) && snaps_.size() > 1) {
            const std::size_t sz = snaps_.front().field.cells.size();
            bytes_ = (bytes_ > sz) ? bytes_ - sz : 0;   // saturating: never wrap
            snaps_.pop_front();
        }
    }

    [[nodiscard]] std::size_t depth() const { return snaps_.size(); }
    [[nodiscard]] bool        empty() const { return snaps_.empty(); }
    [[nodiscard]] std::size_t bytes() const { return bytes_; }

    // Index 0 is the oldest retained snapshot.
    [[nodiscard]] const Field* at(std::size_t i) const {
        return i < snaps_.size() ? &snaps_[i].field : nullptr;
    }
    [[nodiscard]] std::uint64_t generation_at(std::size_t i) const {
        return i < snaps_.size() ? snaps_[i].gen : 0;
    }

    struct Trace {
        std::deque<double> values;
        double             max_hint = 0.0;
        Metric::Direction  better = Metric::Higher;   // copied from the Metric
    };
    [[nodiscard]] const std::vector<std::string>& names(Axis ax = Axis::Frame) const {
        return (ax == Axis::Epoch) ? eorder_ : order_;
    }
    [[nodiscard]] const Trace* trace(const std::string& n, Axis ax = Axis::Frame) const {
        const auto& m = (ax == Axis::Epoch) ? etraces_ : traces_;
        auto it = m.find(n);
        return it == m.end() ? nullptr : &it->second;
    }
    // How many completed epochs have been recorded, and which epoch each
    // sample was taken at.
    [[nodiscard]] std::size_t epoch_samples() const { return epochAt_.size(); }
    [[nodiscard]] int epoch_index(std::size_t i) const {
        return i < epochAt_.size() ? epochAt_[i] : 0;
    }

    // ── keeping one run to compare the next against ─────────────────────────
    //
    // A single curve tells you a run happened. Two tell you whether the change
    // you just made helped, and that is the only question a bench exists to
    // answer. Without this the workflow is: train, screenshot, change a knob,
    // retrain, and compare the new curve against a memory of the old one.
    //
    // Deliberately NOT cleared by clear(). Reset is exactly the moment you want
    // the old curve kept — you are about to run the comparison.
    void keep_baseline() {
        baseline_      = etraces_;
        baselineOrder_ = eorder_;
        baselineAt_    = epochAt_;
    }
    void clear_baseline() { baseline_.clear(); baselineOrder_.clear(); baselineAt_.clear(); }
    [[nodiscard]] bool has_baseline() const { return !baselineAt_.empty(); }
    [[nodiscard]] std::size_t baseline_samples() const { return baselineAt_.size(); }
    [[nodiscard]] const Trace* baseline_trace(const std::string& n) const {
        auto it = baseline_.find(n);
        return it == baseline_.end() ? nullptr : &it->second;
    }
    [[nodiscard]] double baseline_best(const std::string& n) const {
        const Trace* t = baseline_trace(n);
        if (!t || t->values.empty()) return 0.0;
        double b = t->values.front();
        if (t->better == Metric::Neither) return t->values.back();
        for (double v : t->values)
            b = (t->better == Metric::Higher) ? std::max(b, v) : std::min(b, v);
        return b;
    }
    // Which way is better for a named series, so a caller can decide whether a
    // best line or a comparison means anything at all.
    [[nodiscard]] Metric::Direction direction(const std::string& n, Axis ax = Axis::Frame) const {
        const Trace* t = trace(n, ax);
        return t ? t->better : Metric::Higher;
    }

private:
    void recordEpoch(const Sim& sim, int e) {
        for (const auto& m : sim.metrics()) {
            auto it = etraces_.find(m.name);
            if (it == etraces_.end()) {
                if (eorder_.size() >= kMaxTraces) continue;
                eorder_.push_back(m.name);
                it = etraces_.emplace(m.name, Trace{}).first;
                it->second.max_hint      = m.max;
                it->second.better        = m.better;
            }
            auto& t = it->second;
            t.values.push_back(m.value);
            if (t.values.size() > kTraceCap) t.values.pop_front();
        }
        epochAt_.push_back(e);
        if (epochAt_.size() > kTraceCap) epochAt_.pop_front();
    }

public:

private:
    static constexpr std::size_t kTraceCap  = 4096;
    static constexpr std::size_t kMaxTraces = 12;    // a plugin cannot flood us
    static constexpr std::size_t kMaxSnaps  = 3000;  // bound count as well as bytes
    struct Snap { std::uint64_t gen; Field field; };

    std::deque<Snap>                            snaps_;
    std::unordered_map<std::string, Trace>      traces_;
    std::vector<std::string>                    order_;   // stable plot order
    std::unordered_map<std::string, Trace>      etraces_; // sampled per epoch
    std::vector<std::string>                    eorder_;
    std::deque<int>                             epochAt_; // epoch number per sample
    std::unordered_map<std::string, Trace>      baseline_;  // a kept run, to compare against
    std::vector<std::string>                    baselineOrder_;
    std::deque<int>                             baselineAt_;
    int  lastEpoch_  = 0;
    std::size_t budget_  = 64u * 1024u * 1024u;
    std::size_t bytes_   = 0;
    bool recordSnapshots_ = true;
    std::size_t samples_ = 0;
    int         every_   = 4;
    int         minIntervalMs_ = 33;      // ~30 snapshots/sec, whatever the step rate
    std::uint64_t lastSnapGen_ = 0;
    bool        haveClock_ = false;
    std::chrono::steady_clock::time_point lastSnapAt_{};
};

} // namespace bench
