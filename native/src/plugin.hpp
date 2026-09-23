// plugin.hpp — sims as hot-reloadable DLLs.
//
// This is the feature that makes it an IDE rather than a viewer: write a rule,
// rebuild, and watch it change without restarting or losing your place.
//
// ── the contract ────────────────────────────────────────────────────────────
// A plugin is a DLL that exports one C function:
//
//     extern "C" __declspec(dllexport) bench::Sim* bench_create_sim();
//
// It must be built with the SAME compiler and standard library as the
// workbench, because it hands back a C++ object with virtual calls across the
// boundary. That is a real constraint, not an oversight: the alternative is a
// flattened C ABI that would make writing a sim significantly less pleasant,
// and the whole point of this bench is that a new rule is short.
//
// ── the shadow-copy trick ───────────────────────────────────────────────────
// Windows locks a loaded DLL, so a rebuild into the same path fails while the
// workbench is running. The host copies each DLL to a shadow file and loads
// THAT, leaving the original free to be overwritten. Standard practice for live
// reload, and the reason this works at all.

#pragma once
#include "sim.hpp"
#include "file_paths.hpp"
#include <string>
#include <vector>
#include <filesystem>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#undef near
#undef far
#endif

namespace bench {

using CreateSimFn = Sim* (*)();
inline constexpr const char* kCreateSymbol = "bench_create_sim";

#ifdef _WIN32

class Plugin {
public:
    Plugin() = default;
    ~Plugin() { unload(); }
    Plugin(const Plugin&) = delete;
    Plugin& operator=(const Plugin&) = delete;

    [[nodiscard]] const std::string& path()  const { return path_; }
    [[nodiscard]] const std::string& error() const { return err_;  }
    [[nodiscard]] bool               ok()    const { return lib_ != nullptr; }

    // Load (or reload) the DLL at `path`. Returns true if a fresh sim can be made.
    bool load(const std::string& path) {
        unload();
        path_ = path;
        err_.clear();

        static unsigned serial=0;
        shadow_ = path + ".loaded-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(++serial)+".dll";
        // Copy so the original stays writable while we hold this one open.
        if (!CopyFileW(bench::path_from_utf8(path).c_str(), bench::path_from_utf8(shadow_).c_str(), TRUE)) {
            err_ = "could not shadow-copy the dll (is it still being written?)";
            return false;
        }
        lib_ = LoadLibraryW(bench::path_from_utf8(shadow_).c_str());
        if (!lib_) { err_ = "LoadLibrary failed"; return false; }

        create_ = reinterpret_cast<CreateSimFn>(
            reinterpret_cast<void*>(GetProcAddress(lib_, kCreateSymbol)));
        if (!create_) {
            err_ = std::string("dll has no ") + kCreateSymbol +
                   " - did you forget the extern \"C\" export?";
            unload();
            return false;
        }
        stamp_ = write_time(path_);
        return true;
    }

    void unload() {
        create_ = nullptr;
        if (lib_) { FreeLibrary(lib_); lib_ = nullptr; }
        if (!shadow_.empty()) { DeleteFileW(bench::path_from_utf8(shadow_).c_str()); shadow_.clear(); }
    }

    // Has the source DLL been rewritten since we loaded it?
    [[nodiscard]] bool changed_on_disk() const {
        if (path_.empty()) return false;
        const FILETIME t = write_time(path_);
        return CompareFileTime(&t, &stamp_) != 0 && t.dwLowDateTime != 0;
    }

    SimPtr make() {
        if (!create_) return nullptr;
        Sim* raw = create_();
        return SimPtr(raw);
    }

    static FILETIME write_time(const std::string& p) {
        WIN32_FILE_ATTRIBUTE_DATA d{};
        if (!GetFileAttributesExW(bench::path_from_utf8(p).c_str(), GetFileExInfoStandard, &d)) return FILETIME{};
        return d.ftLastWriteTime;
    }

    // Every *.dll in a folder, excluding our own shadow copies.
    static std::vector<std::string> scan(const std::string& folder) {
        std::vector<std::string> out;
        WIN32_FIND_DATAA fd{};
        const std::string pat = folder + "\\*.dll";
        HANDLE h = FindFirstFileA(pat.c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return out;
        do {
            const std::string name = fd.cFileName;
            if (name.find(".loaded-")!=std::string::npos)continue;
            if (name.size() > 11 &&
                name.compare(name.size() - 11, 11, ".loaded.dll") == 0) continue;
            out.push_back(folder + "\\" + name);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
        return out;
    }

private:
    HMODULE     lib_    = nullptr;
    CreateSimFn create_ = nullptr;
    std::string path_, shadow_, err_;
    FILETIME    stamp_{};
};

#else  // non-Windows: compiles, does nothing, so the rest of the bench is portable

class Plugin {
public:
    [[nodiscard]] const std::string& path()  const { return path_; }
    [[nodiscard]] const std::string& error() const { return err_;  }
    [[nodiscard]] bool ok() const { return false; }
    bool load(const std::string& p) {
        path_ = p; err_ = "hot reload is Win32-only for now"; return false;
    }
    void unload() {}
    [[nodiscard]] bool changed_on_disk() const { return false; }
    SimPtr make() { return nullptr; }
    static std::vector<std::string> scan(const std::string&) { return {}; }
private:
    std::string path_, err_ = "hot reload is Win32-only for now";
};

#endif

} // namespace bench
