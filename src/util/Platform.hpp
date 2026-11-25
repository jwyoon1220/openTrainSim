#pragma once
// The few things that differ between Linux and Windows outside the plugin host process
// (sim/Plugin.cpp): where the executable lives, per-user directories, console setup.
#include <filesystem>
#include <string>
#include <vector>

namespace ots {

// The running executable (empty if unknown).
std::filesystem::path executablePath();
// Directory of the running executable (current directory if unknown).
std::filesystem::path executableDir();
// Per-user cache directory for openTrainSim (XDG_CACHE_HOME / %LOCALAPPDATA%).
std::filesystem::path userCacheDir();
// The user's home (HOME / %USERPROFILE%); empty if unknown.
std::filesystem::path userHomeDir();
// Runs a program (looked up in PATH) with arguments, no shell, and waits. Returns its exit
// code, or -1 if it could not be started.
int runProcess(const std::vector<std::string>& args);
// Windows: UTF-8 console output. No-op elsewhere.
void initConsole();

} // namespace ots
