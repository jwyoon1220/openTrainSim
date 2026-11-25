#include "util/Platform.hpp"

#include <cstdlib>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char** environ;
#endif

namespace ots {

namespace {
std::filesystem::path envPath(const char* name) {
#ifdef _WIN32
    // Wide API: the narrow environment is in the ANSI code page.
    std::wstring wname(name, name + std::char_traits<char>::length(name));
    const DWORD n = GetEnvironmentVariableW(wname.c_str(), nullptr, 0);
    if (n == 0) return {};
    std::wstring v(n, L'\0');
    const DWORD m = GetEnvironmentVariableW(wname.c_str(), v.data(), n);
    v.resize(m);
    return v.empty() ? std::filesystem::path() : std::filesystem::path(v);
#else
    const char* v = std::getenv(name);
    return v && *v ? std::filesystem::path(v) : std::filesystem::path();
#endif
}
} // namespace

std::filesystem::path executablePath() {
#ifdef _WIN32
    std::wstring buf(MAX_PATH, L'\0');
    for (;;) {
        const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
        if (n == 0) return {};
        if (n < buf.size()) {
            buf.resize(n);
            return std::filesystem::path(buf);
        }
        buf.resize(buf.size() * 2);
    }
#else
    std::error_code ec;
    std::filesystem::path p = std::filesystem::read_symlink("/proc/self/exe", ec);
    return ec ? std::filesystem::path() : p;
#endif
}

std::filesystem::path executableDir() {
    const std::filesystem::path exe = executablePath();
    if (!exe.empty()) return exe.parent_path();
    std::error_code ec;
    return std::filesystem::current_path(ec);
}

std::filesystem::path userHomeDir() {
#ifdef _WIN32
    return envPath("USERPROFILE");
#else
    return envPath("HOME");
#endif
}

std::filesystem::path userCacheDir() {
#ifdef _WIN32
    if (auto p = envPath("LOCALAPPDATA"); !p.empty()) return p / "openTrainSim" / "cache";
#else
    if (auto p = envPath("XDG_CACHE_HOME"); !p.empty()) return p / "openTrainSim";
    if (auto p = envPath("HOME"); !p.empty()) return p / ".cache" / "openTrainSim";
#endif
    std::error_code ec;
    return std::filesystem::temp_directory_path(ec) / "openTrainSim-cache";
}

void initConsole() {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
}

int runProcess(const std::vector<std::string>& args) {
    if (args.empty()) return -1;
#ifdef _WIN32
    auto widen = [](const std::string& u) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, u.data(), int(u.size()), nullptr, 0);
        std::wstring w(std::size_t(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, u.data(), int(u.size()), w.data(), n);
        return w;
    };
    std::wstring cmd;
    for (const std::string& a : args) { // CommandLineToArgvW quoting
        if (!cmd.empty()) cmd += L' ';
        cmd += L'"';
        std::size_t slashes = 0;
        for (wchar_t c : widen(a)) {
            if (c == L'\\') {
                ++slashes;
                continue;
            }
            cmd.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
            slashes = 0;
            cmd += c;
        }
        cmd.append(slashes * 2, L'\\');
        cmd += L'"';
    }
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
#else
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    if (posix_spawnp(&pid, argv[0], nullptr, nullptr, argv.data(), environ) != 0) return -1;
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace ots
