// SPDX-License-Identifier: GPL-3.0-or-later
// Embodied Brian2 / Shiu-reference controller bridge for fly actor hosts.
// Speaks Observation/Action via a bounded Python subprocess (JSON lines).
// FlyWire NC stays optional discovery only — never required, never installer.
#pragma once
#include "fly.hpp"
#include "fly_pack.hpp"
#include "file_paths.hpp"
#include "paths.hpp"
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <sstream>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cstdio>
#  include <unistd.h>
#  include <sys/wait.h>
#  include <signal.h>
#  include <poll.h>
#endif

namespace bench::fly::brian2ref {

inline constexpr const char* kBackend = "shiu-brian2";
inline constexpr const char* kEnvPython = "LIFESIM_BRIAN2_PYTHON";
inline constexpr const char* kEnvBridge = "LIFESIM_BRIAN2_BRIDGE";
inline constexpr const char* kMapping = "lif-rate-v1";

inline std::filesystem::path search_up(const std::filesystem::path& rel) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path cur = fs::current_path(ec);
    for (int i = 0; i < 8 && !ec; ++i) {
        const fs::path cand = cur / rel;
        if (fs::is_regular_file(cand)) return cand;
        if (!cur.has_parent_path() || cur == cur.parent_path()) break;
        cur = cur.parent_path();
    }
    return {};
}

inline std::filesystem::path default_python() {
    if (const char* e = std::getenv(kEnvPython); e && *e)
        return bench::path_from_utf8(e);
#ifdef _WIN32
    try {
        const auto via_paths = bench::path_from_utf8(bench::Paths::get().root())
            / "userdata" / "brian2-venv" / "Scripts" / "python.exe";
        if (std::filesystem::is_regular_file(via_paths)) return via_paths;
    } catch (...) {}
    return search_up(bench::path_from_utf8("userdata/brian2-venv/Scripts/python.exe"));
#else
    return search_up(bench::path_from_utf8("userdata/brian2-venv/bin/python"));
#endif
}

inline std::filesystem::path default_bridge_script() {
    if (const char* e = std::getenv(kEnvBridge); e && *e)
        return bench::path_from_utf8(e);
#ifdef _WIN32
    try {
        const auto via_paths = bench::path_from_utf8(bench::Paths::get().root())
            / "native" / "scripts" / "brian2_controller_bridge.py";
        if (std::filesystem::is_regular_file(via_paths)) return via_paths;
    } catch (...) {}
#endif
    const auto up = search_up(bench::path_from_utf8("native/scripts/brian2_controller_bridge.py"));
    if (!up.empty()) return up;
    return bench::path_from_utf8("native/scripts/brian2_controller_bridge.py");
}

inline bool runtime_present() {
    const auto py = default_python();
    const auto br = default_bridge_script();
    return !py.empty() && std::filesystem::is_regular_file(py)
        && !br.empty() && std::filesystem::is_regular_file(br);
}

// Identity fields when the optional reference is selected. dataset_hash comes
// from the local FlyWire pack when present; empty means synthetic / no NC data.
struct ReferenceIdentity {
    static constexpr unsigned contract_version = Observation::version;
    static constexpr const char* backend = kBackend;
    static constexpr const char* body = Identity::body;
    static constexpr const char* mapping = kMapping;
    static std::string dataset_hash() { return pack::dataset_hash(); }
};

// Suggested command line for a bounded subprocess check (not launched here):
//   <brian2-python> native/scripts/shiu_reference_smoke.py
inline std::string smoke_command() {
    const auto py = default_python();
    if (py.empty()) return {};
#ifdef _WIN32
    return path_text(py) + " native\\\\scripts\\\\shiu_reference_smoke.py";
#else
    return path_text(py) + " native/scripts/shiu_reference_smoke.py";
#endif
}

namespace detail {

inline bool extract_number(std::string_view json, std::string_view key, double& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) return false;
    const auto colon = json.find(':', pos + needle.size());
    if (colon == std::string_view::npos) return false;
    size_t i = colon + 1;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    try {
        size_t used = 0;
        out = std::stod(std::string(json.substr(i)), &used);
        return used > 0;
    } catch (...) {
        return false;
    }
}

inline bool extract_bool(std::string_view json, std::string_view key, bool& out) {
    const std::string needle = "\"" + std::string(key) + "\"";
    const auto pos = json.find(needle);
    if (pos == std::string_view::npos) return false;
    const auto colon = json.find(':', pos + needle.size());
    if (colon == std::string_view::npos) return false;
    const auto t = json.find("true", colon);
    const auto f = json.find("false", colon);
    if (t != std::string_view::npos && (f == std::string_view::npos || t < f)) {
        out = true; return true;
    }
    if (f != std::string_view::npos) { out = false; return true; }
    return false;
}

inline bool extract_uint(std::string_view json, std::string_view key, std::uint64_t& out) {
    double d = 0;
    if (!extract_number(json, key, d) || d < 0) return false;
    out = static_cast<std::uint64_t>(d);
    return true;
}

} // namespace detail

// Persistent JSON-line subprocess implementing Controller. Hosts must keep
// calls bounded (timeout_ms) and destroy the controller on world switch.
class Brian2Controller final : public Controller {
public:
    int timeout_ms = 2000;
    double odor_gain = 1;
    double light_gain = 0;

    Brian2Controller() = default;
    Brian2Controller(const Brian2Controller&) = delete;
    Brian2Controller& operator=(const Brian2Controller&) = delete;
    ~Brian2Controller() override { stop(); }

    const char* backend() const override { return kBackend; }
    bool available() const { return runtime_present(); }

    void reset(std::uint64_t seed) override {
        seed_ = seed;
        if (!ensure_started()) return;
        std::ostringstream req;
        req.precision(17);
        req << "{\"cmd\":\"reset\",\"seed\":" << seed_
            << ",\"odor_gain\":" << odor_gain
            << ",\"light_gain\":" << light_gain << "}\n";
        std::string resp;
        if (!transact(req.str(), resp)) { stop(); return; }
        bool ok = false;
        detail::extract_bool(resp, "ok", ok);
        if (!ok) stop();
    }

    Action act(const Observation& o) override {
        Action fallback{Observation::version, o.tick, 0.0, 0.0};
        if (!ensure_started()) return fallback;
        std::ostringstream req;
        req.precision(17);
        req << "{\"cmd\":\"act\",\"tick\":" << o.tick
            << ",\"seconds\":" << o.seconds
            << ",\"interval\":" << o.interval
            << ",\"odor\":[" << o.odor[0] << "," << o.odor[1] << "]"
            << ",\"light\":[" << o.light[0] << "," << o.light[1] << "]"
            << ",\"clearance\":[" << o.clearance[0] << "," << o.clearance[1]
            << "," << o.clearance[2] << "]"
            << ",\"contact\":" << (o.contact ? "true" : "false") << "}\n";
        std::string resp;
        if (!transact(req.str(), resp)) { stop(); return fallback; }
        bool ok = false;
        detail::extract_bool(resp, "ok", ok);
        double forward = 0, turn = 0;
        std::uint64_t tick = o.tick;
        if (!ok || !detail::extract_number(resp, "forward", forward)
            || !detail::extract_number(resp, "turn", turn)) {
            stop();
            return fallback;
        }
        detail::extract_uint(resp, "tick", tick);
        Action a{Observation::version, tick, forward, turn};
        if (!valid(a, o.tick)) return fallback;
        return a;
    }

    void stop() {
#ifdef _WIN32
        if (proc_) {
            if (stdin_w_ != INVALID_HANDLE_VALUE) {
                CloseHandle(stdin_w_);
                stdin_w_ = INVALID_HANDLE_VALUE;
            }
            if (stdout_r_ != INVALID_HANDLE_VALUE) {
                CloseHandle(stdout_r_);
                stdout_r_ = INVALID_HANDLE_VALUE;
            }
            TerminateProcess(proc_, 1);
            WaitForSingleObject(proc_, 1000);
            CloseHandle(proc_);
            proc_ = nullptr;
        }
#else
        if (pid_ > 0) {
            if (in_fd_ >= 0) { close(in_fd_); in_fd_ = -1; }
            if (out_fd_ >= 0) { close(out_fd_); out_fd_ = -1; }
            kill(pid_, SIGTERM);
            waitpid(pid_, nullptr, 0);
            pid_ = -1;
        }
#endif
    }

private:
    std::uint64_t seed_ = 1;

#ifdef _WIN32
    HANDLE proc_ = nullptr;
    HANDLE stdin_w_ = INVALID_HANDLE_VALUE;
    HANDLE stdout_r_ = INVALID_HANDLE_VALUE;

    bool ensure_started() {
        if (proc_) return true;
        if (!runtime_present()) return false;
        const auto py = default_python();
        const auto br = default_bridge_script();
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        HANDLE in_r = INVALID_HANDLE_VALUE, in_w = INVALID_HANDLE_VALUE;
        HANDLE out_r = INVALID_HANDLE_VALUE, out_w = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&in_r, &in_w, &sa, 0)) return false;
        if (!CreatePipe(&out_r, &out_w, &sa, 0)) {
            CloseHandle(in_r); CloseHandle(in_w); return false;
        }
        SetHandleInformation(in_w, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(out_r, HANDLE_FLAG_INHERIT, 0);
        STARTUPINFOW si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = in_r;
        si.hStdOutput = out_w;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        PROCESS_INFORMATION pi{};
        std::wstring cmd = L"\"" + py.wstring() + L"\" \"" + br.wstring() + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(0);
        const BOOL ok = CreateProcessW(
            nullptr, buf.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        CloseHandle(in_r);
        CloseHandle(out_w);
        if (!ok) {
            CloseHandle(in_w); CloseHandle(out_r); return false;
        }
        CloseHandle(pi.hThread);
        proc_ = pi.hProcess;
        stdin_w_ = in_w;
        stdout_r_ = out_r;
        return true;
    }

    bool transact(const std::string& req, std::string& resp) {
        resp.clear();
        if (!proc_ || stdin_w_ == INVALID_HANDLE_VALUE || stdout_r_ == INVALID_HANDLE_VALUE)
            return false;
        DWORD written = 0;
        if (!WriteFile(stdin_w_, req.data(), DWORD(req.size()), &written, nullptr)
            || written != req.size())
            return false;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        std::string acc;
        char chunk[512];
        while (std::chrono::steady_clock::now() < deadline) {
            DWORD avail = 0;
            if (!PeekNamedPipe(stdout_r_, nullptr, 0, nullptr, &avail, nullptr))
                return false;
            if (avail == 0) {
                if (WaitForSingleObject(proc_, 0) == WAIT_OBJECT_0) return false;
                Sleep(5);
                continue;
            }
            DWORD got = 0;
            const DWORD n = (std::min)(avail, DWORD(sizeof(chunk)));
            if (!ReadFile(stdout_r_, chunk, n, &got, nullptr) || got == 0) return false;
            acc.append(chunk, chunk + got);
            const auto nl = acc.find('\n');
            if (nl != std::string::npos) {
                resp = acc.substr(0, nl);
                return true;
            }
        }
        return false;
    }
#else
    pid_t pid_ = -1;
    int in_fd_ = -1, out_fd_ = -1;

    bool ensure_started() {
        if (pid_ > 0) return true;
        if (!runtime_present()) return false;
        int in_pipe[2], out_pipe[2];
        if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) return false;
        pid_ = fork();
        if (pid_ < 0) return false;
        if (pid_ == 0) {
            dup2(in_pipe[0], STDIN_FILENO);
            dup2(out_pipe[1], STDOUT_FILENO);
            close(in_pipe[0]); close(in_pipe[1]);
            close(out_pipe[0]); close(out_pipe[1]);
            const auto py = default_python();
            const auto br = default_bridge_script();
            execl(py.c_str(), py.c_str(), br.c_str(), static_cast<char*>(nullptr));
            _exit(127);
        }
        close(in_pipe[0]); close(out_pipe[1]);
        in_fd_ = in_pipe[1];
        out_fd_ = out_pipe[0];
        return true;
    }

    bool transact(const std::string& req, std::string& resp) {
        resp.clear();
        if (pid_ <= 0 || in_fd_ < 0 || out_fd_ < 0) return false;
        if (write(in_fd_, req.data(), req.size()) != ssize_t(req.size())) return false;
        const auto deadline = std::chrono::steady_clock::now()
            + std::chrono::milliseconds(timeout_ms);
        std::string acc;
        char chunk[512];
        while (std::chrono::steady_clock::now() < deadline) {
            pollfd pfd{out_fd_, POLLIN, 0};
            const int pr = poll(&pfd, 1, 20);
            if (pr < 0) return false;
            if (pr == 0) continue;
            const ssize_t got = read(out_fd_, chunk, sizeof(chunk));
            if (got <= 0) return false;
            acc.append(chunk, chunk + got);
            const auto nl = acc.find('\n');
            if (nl != std::string::npos) {
                resp = acc.substr(0, nl);
                return true;
            }
        }
        return false;
    }
#endif
};

inline std::unique_ptr<Controller> try_make_brian2() {
    if (!runtime_present()) return {};
    return std::make_unique<Brian2Controller>();
}

} // namespace bench::fly::brian2ref
