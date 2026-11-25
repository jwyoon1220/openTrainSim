#version 460
#include "common.glsl"
layout(location = 0) in vec2 vUv;
layout(location = 1) flat in float vRepeat;

void main() {
    Material m = MaterialRef(pc.materials).m[pc.material];
    if ((m.flags & (MAT_ALPHATEST | MAT_BLEND)) != 0u) { // see-through texels and glass cast no shadow
        float a = unpackUnorm4x8(m.color).a;
        if ((m.flags & MAT_TEXTURED) != 0u) a *= textureLod(gTextures[nonuniformEXT(m.texture)], vRepeat > 0.5 ? fract(vUv) : vUv, 0.0).a;
        if (a < 0.5) discard;
    }
}
