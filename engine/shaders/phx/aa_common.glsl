#ifndef PHX_AA_COMMON_GLSL
#define PHX_AA_COMMON_GLSL
// Shared helpers for the anti-aliasing passes (taa.comp, fsr_*.comp).
// Reversible per-channel tonemap: temporal blending / FSR filtering in a bounded space,
// so HDR highlights neither flicker nor ring.
vec3 aaCompress(vec3 c) { return c / (1.0 + c); }
vec3 aaExpand(vec3 c) { return c / max(1.0 - c, vec3(1.0 / 65504.0)); }
#endif
