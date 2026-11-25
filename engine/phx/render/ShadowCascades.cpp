#include "phx/render/ShadowCascades.hpp"

#include <algorithm>
#include <cmath>

namespace phx {

bool ShadowCascade::covers(const glm::vec3& c, f32 r) const {
    const glm::vec3 d = c - center;
    return std::abs(glm::dot(d, lightX)) <= radius + r && std::abs(glm::dot(d, lightY)) <= radius + r;
}

void computeShadowCascades(const ShadowCascadeInput& in, std::span<ShadowCascade> out) {
    const u32 n = u32(out.size());
    if (n == 0) return;
    // light basis
    const glm::vec3 Z = -glm::normalize(in.toLight);
    const glm::vec3 ref = std::abs(Z.y) > 0.99f ? glm::vec3(1, 0, 0) : glm::vec3(0, 1, 0);
    const glm::vec3 X = glm::normalize(glm::cross(ref, Z));
    const glm::vec3 Y = glm::cross(Z, X);
    const f32 ty = std::tan(0.5f * in.fovY), tx = ty * in.aspect;
    const f32 k2 = tx * tx + ty * ty;
    const f32 nearP = std::max(in.nearPlane, 0.01f), farP = std::max(in.distance, nearP * 2.0f);
    f32 prev = nearP;
    for (u32 i = 0; i < n; ++i) {
        const f32 t = f32(i + 1) / f32(n);
        const f32 logS = nearP * std::pow(farP / nearP, t), linS = nearP + (farP - nearP) * t;
        const f32 split = i + 1 == n ? farP : in.splitLambda * logS + (1.0f - in.splitLambda) * linS;
        const f32 a = i == 0 ? 0.0f : prev, b = split; // the first cascade starts at the eye
        // minimal sphere around the slice [a, b] (depends on the field of view only)
        f32 c = 0.5f * (a + b) * (1.0f + k2);
        if (c > b) c = b;
        f32 r = std::sqrt((b - c) * (b - c) + b * b * k2);
        r = std::max(r, std::sqrt((c - a) * (c - a) + a * a * k2));
        r = std::ceil(r * 16.0f) / 16.0f; // a constant size: no swimming from rounding
        const f32 texel = 2.0f * r / f32(in.resolution);
        // snap the centre to the texel grid in absolute light space
        const glm::dvec3 abs = in.camera + glm::dvec3(in.forward) * double(c);
        const double sx = std::floor(glm::dot(abs, glm::dvec3(X)) / texel) * texel;
        const double sy = std::floor(glm::dot(abs, glm::dvec3(Y)) / texel) * texel;
        const double sz = glm::dot(abs, glm::dvec3(Z));
        const glm::dvec3 snapped = glm::dvec3(X) * sx + glm::dvec3(Y) * sy + glm::dvec3(Z) * sz;
        const glm::vec3 center = glm::vec3(snapped - in.camera);
        // clip = (dot(p - eye, X) / r, dot(p - eye, Y) / r, dot(p - eye, Z) / depth), eye = centre − Z (r + extent)
        const f32 depth = 2.0f * r + in.casterExtent;
        const glm::vec3 eye = center - Z * (r + in.casterExtent);
        glm::mat4 m(1.0f);
        const glm::vec3 rows[3] = {X / r, Y / r, Z / depth};
        for (int row = 0; row < 3; ++row) {
            m[0][row] = rows[row].x;
            m[1][row] = rows[row].y;
            m[2][row] = rows[row].z;
            m[3][row] = -glm::dot(rows[row], eye);
        }
        m[0][3] = m[1][3] = m[2][3] = 0.0f;
        m[3][3] = 1.0f;
        ShadowCascade& s = out[i];
        s.viewProj = m;
        s.center = center;
        s.radius = r;
        s.nearSplit = a;
        s.farSplit = b;
        s.texelWorld = texel;
        s.lightX = X, s.lightY = Y, s.lightZ = Z;
        prev = split;
    }
}

} // namespace phx
