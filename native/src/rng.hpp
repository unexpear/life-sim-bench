// rng.hpp — a tiny deterministic PRNG.
//
// std::mt19937 would be fine, but its output is only portable if you also pin
// the distribution, and the distributions are not specified bit-for-bit. This
// is xorshift64*, which gives identical soup on every platform and compiler —
// so a self-test that says "this seed produces 1,240 live cells" stays true.

#pragma once
#include <cstdint>

namespace bench {

class Rng {
public:
    explicit Rng(std::uint64_t seed = 0x9E3779B97F4A7C15ull) : s_(seed ? seed : 1) {}

    std::uint64_t next() noexcept {
        s_ ^= s_ >> 12;
        s_ ^= s_ << 25;
        s_ ^= s_ >> 27;
        return s_ * 0x2545F4914F6CDD1Dull;
    }
    // [0,1)
    float unit() noexcept {
        return float(next() >> 40) * (1.0f / 16777216.0f);
    }
    std::uint32_t below(std::uint32_t n) noexcept {
        return std::uint32_t(next() >> 33) % n;
    }
    void reseed(std::uint64_t seed) noexcept { s_ = seed ? seed : 1; }

private:
    std::uint64_t s_;
};

// Turn a sim's fixed base seed and a run index into an independent seed.
//
// Independent, not merely different: base + run gives adjacent streams, and
// xorshift-family generators started at adjacent states produce correlated
// output for a while. Multiplying by the 64-bit golden ratio and mixing the
// high bits down (SplitMix64's finaliser) decorrelates them, which matters
// because the whole point of running several seeds is that they are
// independent samples.
[[nodiscard]] inline std::uint64_t mix_seed(std::uint64_t base, int run) {
    std::uint64_t z = base + 0x9E3779B97F4A7C15ull * std::uint64_t(run < 0 ? 0 : run);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z = z ^ (z >> 31);
    return z ? z : 0x9E3779B97F4A7C15ull;    // xorshift dies at zero
}

} // namespace bench
