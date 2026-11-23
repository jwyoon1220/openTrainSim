#version 460
#include "phx/bindless.glsl"
// Output transform. openBVE lighting is display-referred already (LDR, gamma space), so the
// "tonemap" is a clamp of TEX_HDR (scene, or the DLSS / TAA / FSR / SSAA output).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
void main() { outColor = vec4(clamp(texture(gTextures[TEX_HDR], vUV).rgb, 0.0, 1.0), 1.0); }
