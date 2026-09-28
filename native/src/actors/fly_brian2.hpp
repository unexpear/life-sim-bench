// SPDX-License-Identifier: GPL-3.0-or-later
// Bridge notes + thin helper for calling the Shiu/Brian2 Windows reference.
// The registered flyarena host still uses ReactiveController. This header
// documents how a future controller process can locate the portable venv and
// optional FlyWire pack without embedding NC data in the binary.
#pragma once
#include "fly.hpp"
#include "fly_pack.hpp"
#include <cstdlib>
#include <filesystem>
#include <string>

namespace bench::fly::brian2ref {

inline constexpr const char* kBackend = "shiu-brian2";
inline constexpr const char* kEnvPython = "LIFESIM_BRIAN2_PYTHON";

inline std::filesystem::path default_python() {
    if (const char* e = std::getenv(kEnvPython); e && *e)
        return bench::path_from_utf8(e);
#ifdef _WIN32
    try {
        return bench::path_from_utf8(bench::Paths::get().root())
            / "userdata" / "brian2-venv" / "Scripts" / "python.exe";
    } catch (...) {
        return {};
    }
#else
    return {};
#endif
}

inline bool runtime_present() {
    const auto py = default_python();
    return !py.empty() && std::filesystem::is_regular_file(py);
}

// Identity fields when the optional reference is selected. dataset_hash comes
// from the local FlyWire pack when present; empty means synthetic / no NC data.
struct ReferenceIdentity {
    static constexpr unsigned contract_version = Observation::version;
    static constexpr const char* backend = kBackend;
    static constexpr const char* body = Identity::body;
    static std::string dataset_hash() { return pack::dataset_hash(); }
};

// Suggested command line for a bounded subprocess check (not launched here):
//   <brian2-python> native/scripts/shiu_reference_smoke.py
// Hosts must keep calls bounded and must not block UI shutdown (see RESEARCH-NEXT).
inline std::string smoke_command() {
    const auto py = default_python();
    if (py.empty()) return {};
#ifdef _WIN32
    return path_text(py) + " native\\\\scripts\\\\shiu_reference_smoke.py";
#else
    return path_text(py) + " native/scripts/shiu_reference_smoke.py";
#endif
}

} // namespace bench::fly::brian2ref
