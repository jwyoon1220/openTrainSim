#include "phx/core/Log.hpp"

#include <chrono>
#include <cstdlib>
#include <mutex>

namespace phx::log {
namespace {
std::mutex gMutex;
const auto gStart = std::chrono::steady_clock::now();
} // namespace

void write(Level level, std::string_view msg) {
    static constexpr const char* kTag[] = {"DBG", "INF", "WRN", "ERR"};
    const double t = std::chrono::duration<double>(std::chrono::steady_clock::now() - gStart).count();
    std::lock_guard lock(gMutex);
    std::FILE* out = level >= Level::Warn ? stderr : stdout;
    std::fprintf(out, "[%9.3f %s] %.*s\n", t, kTag[int(level)], int(msg.size()), msg.data());
}
} // namespace phx::log
