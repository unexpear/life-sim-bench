#pragma once
#include <filesystem>
#include <string>
#include <string_view>

namespace bench {
inline std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
}
inline std::string path_text(const std::filesystem::path& path) {
    auto normalized=path.lexically_normal();normalized.make_preferred();
    const auto text=normalized.u8string();return {text.begin(),text.end()};
}
}
