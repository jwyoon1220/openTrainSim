#ifndef PHX_BINDLESS_GLSL
#define PHX_BINDLESS_GLSL
// The engine's single descriptor set (phx::Bindless) and the engine-reserved slots.
// Buffers are reached through buffer device addresses; every pipeline shares one layout
// with a 128-byte push-constant range (VK_SHADER_STAGE_ALL) that each shader declares itself.
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_scalar_block_layout : require
#extension GL_EXT_shader_explicit_arithmetic_types_int64 : require

layout(set = 0, binding = 0) uniform sampler2D gTextures[];
layout(set = 0, binding = 1, rgba16f) uniform image2D gStorage[];
layout(set = 0, binding = 2, r32f) uniform image2D gStorageR32[];

#include "post_slots.glsl"
#endif
