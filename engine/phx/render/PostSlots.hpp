#pragma once
// Bindless slots reserved by the engine's post-processing modules (Upscaler, AntiAliasing,
// Bloom, Ssao). Mirrors engine/shaders/phx/post_slots.glsl.
//
// The engine owns sampled slots [kEngineSampledBegin, Bindless::kMaxSampled) and storage
// slots [kEngineStorageBegin, Bindless::kMaxStorage); a game is free to use everything
// below those. The game writes the scene inputs (kTexHdr, kTexDepth, kTexMotion,
// kStorMotion); the modules write their own intermediate targets.
#include "phx/core/Types.hpp"

namespace phx {

inline constexpr u32 kEngineSampledBegin = 1000;
inline constexpr u32 kEngineStorageBegin = 48;

enum PostSlot : u32 {
    // Sampled (gTextures)
    kTexHdr = 1000,      // game: output-resolution HDR scene (Bloom input, tonemap input)
    kTexDepth = 1001,    // game: render-resolution depth, reverse-Z (TAA)
    kTexScene = 1002,    // AntiAliasing: render-resolution HDR scene (TAA / SSAA input)
    kTexHistory0 = 1003, // + 0/1: TAA history ping-pong
    kTexEasu = 1005,     // FSR 1 EASU output
    kTexLdr = 1006,      // tonemapped image (FXAA input)
    kTexMotion = 1007,   // game: motion vectors, render resolution, previous − current (UV)
    kTexBloom0 = 1008,   // + level (Bloom::kLevels)
    kTexAo = 1016,       // Ssao: raw ambient occlusion (render resolution)
    // Storage (gStorage, rgba16f)
    kStorMotion = 48,    // game: motion vector output
    kStorHistory0 = 49,  // + 0/1
    kStorEasu = 51,
    kStorAAOut = 52,
    kStorBloom0 = 53,    // + level
    kStorAo = 60,        // Ssao: raw ambient occlusion
    kStorAoTarget = 61,  // game: render-resolution HDR scene the occlusion is applied to (Ssao)
};

} // namespace phx
