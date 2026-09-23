// paths.hpp — where things are, resolved from the executable, never from cwd.
//
// A desktop app gets launched by double-click, from a shortcut, from a taskbar
// pin, from a different drive. The working directory in each case is different
// and mostly not what you'd hope. Anything that resolves "../plugins" against
// cwd works exactly once, from the terminal you built in.
//
// So: find the exe, walk up until the project root is recognisable, and hand
// out absolute paths from there.

#pragma once
#include <string>
#include "file_paths.hpp"
#include <filesystem>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#endif

namespace bench {

class Paths {
public:
    static Paths& get() { static Paths p; return p; }

    [[nodiscard]] const std::string& root()    const { return root_;    }  // repo root
    [[nodiscard]] const std::string& plugins() const { return plugins_; }
    [[nodiscard]] const std::string& src()     const { return src_;     }
    [[nodiscard]] const std::string& exeDir()  const { return exeDir_;  }
    [[nodiscard]] const std::string& data() const { return data_; }
    [[nodiscard]] bool installed() const { return installed_; }
    [[nodiscard]] std::string projects() const { return data_ + "/Simulations"; }
    [[nodiscard]] std::string templates() const { return root_ + "/templates"; }
    [[nodiscard]] std::string starter() const { return installed_?root_+"/assets/example_rule.cpp":root_+"/plugins/example_rule.cpp"; }

    [[nodiscard]] std::string inRoot(const std::string& rel)    const { return root_ + "\\" + rel; }
    [[nodiscard]] std::string inPlugins(const std::string& rel) const { return plugins_ + "\\" + rel; }
    [[nodiscard]] std::string inSrc(const std::string& rel)     const { return src_ + "\\" + rel; }

private:
    Paths() {
#ifdef _WIN32
        wchar_t buf[32768]{};
        GetModuleFileNameW(nullptr, buf, 32768);
        std::string exe = utf8(std::filesystem::path(buf));
        const auto slash = exe.find_last_of("\\/");
        exeDir_ = (slash == std::string::npos) ? "." : exe.substr(0, slash);

        // Walk up looking for the folder that has both plugins\ and native\src.
        // Four levels covers native\, native\build\, native\build\Release\ etc.
        std::string dir = exeDir_;
        for (int i = 0; i < 5; ++i) {
            if (isRoot(dir)) { root_ = dir; break; }
            const auto up = dir.find_last_of("\\/");
            if (up == std::string::npos) break;
            dir = dir.substr(0, up);
        }
        installed_=std::filesystem::is_regular_file(bench::path_from_utf8(exeDir_)/"workbench.install");
        if(installed_)root_=utf8(bench::path_from_utf8(exeDir_).parent_path());
        if (root_.empty()) root_ = exeDir_;    // fall back rather than fail
        wchar_t overrideDir[32768]{};
        const DWORD overrideSize=GetEnvironmentVariableW(L"LIFESIM_USER_DATA",overrideDir,32768);
        if(overrideSize>0&&overrideSize<32768)data_=utf8(std::filesystem::path(overrideDir));
        else if(installed_) {
            wchar_t local[32768]{};const DWORD size=GetEnvironmentVariableW(L"LOCALAPPDATA",local,32768);
            if(!size||size>=32768)throw std::runtime_error("Windows LocalAppData folder is unavailable.");
            data_=utf8(std::filesystem::path(local)/"LifeSimWorkbench");
        } else data_=root_+"/userdata";
#else
        exeDir_ = "."; root_ = "..";
        data_=root_+"/userdata";
#endif
        plugins_ = installed_?data_+"/Plugins":root_+"/plugins";
        src_     = root_ + "/native/src";
        std::error_code ec;
        std::filesystem::create_directories(bench::path_from_utf8(projects()),ec);
        std::filesystem::create_directories(bench::path_from_utf8(plugins_),ec);
    }

    static bool isRoot(const std::string& d) {
#ifdef _WIN32
        const DWORD a = GetFileAttributesW(bench::path_from_utf8(d + "/plugins").c_str());
        const DWORD b = GetFileAttributesW(bench::path_from_utf8(d + "/native/src").c_str());
        return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)
            && b != INVALID_FILE_ATTRIBUTES && (b & FILE_ATTRIBUTE_DIRECTORY);
#else
        (void)d; return false;
#endif
    }

    static std::string utf8(const std::filesystem::path& p) {return path_text(p);}
    std::string exeDir_, root_, plugins_, src_,data_;
    bool installed_=false;
};

} // namespace bench
