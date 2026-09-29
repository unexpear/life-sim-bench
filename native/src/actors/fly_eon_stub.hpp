// SPDX-License-Identifier: GPL-3.0-or-later
// Compileable Eon fly-brain host stub. Not an accelerated runtime and not
// linked to CUDA/WSL packs. See native/EON-DOOMFLY-WINDOWS.md.
#pragma once
#include "fly.hpp"
#include "fly_pack.hpp"
#include <string>

namespace bench::fly::eonstub {

inline constexpr const char* kBackend = "eon-fly-brain-stub";
inline constexpr const char* kPin = "a3db62f9436074e485c0278290c2164ed6150808";

struct Identity {
    static constexpr unsigned contract_version = Observation::version;
    static constexpr const char* backend = kBackend;
    static constexpr const char* body = fly::Identity::body;
    static constexpr const char* upstream_pin = kPin;
    static std::string dataset_hash() { return pack::dataset_hash(); }
};

// Placeholder controller: refuses to invent actions. Hosts that select Eon
// before a real runtime is present keep the body still (safe zero action).
class StubController final : public Controller {
public:
    void reset(std::uint64_t) override {}
    Action act(const Observation& o) override {
        return {Observation::version, o.tick, 0.0, 0.0};
    }
    const char* backend() const override { return kBackend; }
    static constexpr bool windows_runtime_ready = false;
    static constexpr const char* blocker =
        "Eon documents Ubuntu/WSL2 (+ optional CUDA); not packaged for Win32 yet.";
};

} // namespace bench::fly::eonstub
