// SPDX-License-Identifier: GPL-3.0-or-later
// Optional local FlyWire (CC BY-NC) pack discovery for fly actor Identity.
// Data lives under userdata/packs/flywire-nc/ (or LIFESIM_FLYWIRE_PACK).
// Never ship this pack in the GPL installer — see native/PACKS.md.
#pragma once
#include "file_paths.hpp"
#include "paths.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace bench::fly::pack {

inline constexpr const char* kRelPack = "userdata/packs/flywire-nc";
inline constexpr const char* kEnvPack = "LIFESIM_FLYWIRE_PACK";

inline std::filesystem::path default_dir() {
#ifdef _WIN32
    if (const char* e = std::getenv(kEnvPack); e && *e)
        return bench::path_from_utf8(e);
    try {
        return bench::path_from_utf8(bench::Paths::get().root()) / "userdata" / "packs" / "flywire-nc";
    } catch (...) {
        return bench::path_from_utf8(kRelPack);
    }
#else
    if (const char* e = std::getenv(kEnvPack); e && *e)
        return bench::path_from_utf8(e);
    return bench::path_from_utf8(kRelPack);
#endif
}

inline bool present(const std::filesystem::path& dir = default_dir()) {
    return std::filesystem::is_regular_file(dir / "MANIFEST.json")
        && std::filesystem::is_regular_file(dir / "Connectivity_783.parquet");
}

// Pull "dataset_hash" from MANIFEST.json without a JSON library (single string field).
inline std::string dataset_hash(const std::filesystem::path& dir = default_dir()) {
    if (!present(dir)) return {};
    std::ifstream in(dir / "MANIFEST.json", std::ios::binary);
    if (!in) return {};
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::string key = "\"dataset_hash\"";
    const auto pos = text.find(key);
    if (pos == std::string::npos) return {};
    const auto colon = text.find(':', pos + key.size());
    if (colon == std::string::npos) return {};
    const auto q1 = text.find('"', colon + 1);
    if (q1 == std::string::npos) return {};
    const auto q2 = text.find('"', q1 + 1);
    if (q2 == std::string::npos || q2 <= q1 + 1) return {};
    return text.substr(q1 + 1, q2 - q1 - 1);
}

inline std::string identity_label(const std::filesystem::path& dir = default_dir()) {
    const auto h = dataset_hash(dir);
    if (h.empty()) return "dataset_hash=";
    // Short prefix for UI/subtitle; full hash stays in MANIFEST / metrics.
    return "dataset_hash=" + h.substr(0, 12);
}

} // namespace bench::fly::pack
