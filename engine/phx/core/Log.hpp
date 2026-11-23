#pragma once
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <format>
#include <string_view>

namespace phx::log {
enum class Level { Debug, Info, Warn, Error };

void write(Level level, std::string_view msg);

template <class... A> void debug(std::format_string<A...> f, A&&... a) { write(Level::Debug, std::format(f, std::forward<A>(a)...)); }
template <class... A> void info(std::format_string<A...> f, A&&... a) { write(Level::Info, std::format(f, std::forward<A>(a)...)); }
template <class... A> void warn(std::format_string<A...> f, A&&... a) { write(Level::Warn, std::format(f, std::forward<A>(a)...)); }
template <class... A> void error(std::format_string<A...> f, A&&... a) { write(Level::Error, std::format(f, std::forward<A>(a)...)); }
} // namespace phx::log

#define PHX_FATAL(...)                                                         \
    do {                                                                       \
        ::phx::log::error(__VA_ARGS__);                                        \
        std::abort();                                                          \
    } while (0)
