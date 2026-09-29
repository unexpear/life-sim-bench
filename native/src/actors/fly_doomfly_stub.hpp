// SPDX-License-Identifier: GPL-3.0-or-later
// Compileable DOOMFLY adapter stub. Documents the Windows DLL blocker from
// doom/build_kernel.py at pin 71ecf53d… — no libneural.dll branch.
// Does not load MaleCNS, does not claim survival validation.
#pragma once
#include "fly.hpp"
#include <string>

namespace bench::fly::doomflystub {

inline constexpr const char* kBackend = "doomfly-adapter-stub";
inline constexpr const char* kPin = "71ecf53d78eaffaf1a57ed7b0ccf5d458abc9f33";

struct Identity {
    static constexpr unsigned contract_version = Observation::version;
    static constexpr const char* backend = kBackend;
    static constexpr const char* body = fly::Identity::body;
    static constexpr const char* upstream_pin = kPin;
    static constexpr const char* dataset_hash = ""; // MaleCNS is a separate pack
};

struct WindowsBlocker {
    static constexpr bool has_dll_branch = false;
    static constexpr const char* builder =
        "doom/build_kernel.py emits libneural.dylib (darwin) or libneural.so "
        "(else) via clang++ -shared -fPIC — no Windows .dll path.";
    static constexpr const char* next_step =
        "Port build_kernel.py / kernel.cpp for MSVC or MinGW DLL, or run under Linux/WSL.";
};

// Study-only controller. Safe zero actions until a native kernel exists.
class StubController final : public Controller {
public:
    void reset(std::uint64_t) override {}
    Action act(const Observation& o) override {
        return {Observation::version, o.tick, 0.0, 0.0};
    }
    const char* backend() const override { return kBackend; }
    static constexpr bool windows_runtime_ready = false;
};

} // namespace bench::fly::doomflystub
