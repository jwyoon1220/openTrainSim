#pragma once
#include <cstddef>
#include <cstdint>

namespace phx {
using i8 = std::int8_t;
using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using i64 = std::int64_t;
using f32 = float;
using f64 = double;

// Destructive interference size; hard-coded because GCC warns on the std constant's ABI instability.
inline constexpr std::size_t kCacheLine = 64;
} // namespace phx
