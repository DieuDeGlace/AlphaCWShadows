#pragma once
#include <algorithm>
#include <cmath>
// Adapted from Steam MoonandShadow's MIT-licensed MoonRules.h.
// Alpha's native sun vector is the clock-derived phase signal. No time writes.
namespace alphaMoon {
struct Phase {
    float weight = 0, direction[3]{}, strength = 0;
};
inline float smooth(float x) {
    x = std::clamp(x, 0.f, 1.f);
    return x * x * (3 - 2 * x);
}
inline Phase phase(const float *sun, float nightStrength) {
    Phase p;
    float n = std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]);
    if (!std::isfinite(n) || n < .001f)
        return p;
    for (int i = 0; i < 3; i++)
        p.direction[i] = -sun[i] / n;
    p.weight = smooth(p.direction[2] / .5f);
    p.strength = nightStrength * p.weight * smooth(p.direction[2] / .5f);
    return p;
}
inline void lighting(float out[24], const float native[24], Phase p, float amount) {
    std::copy(native, native + 24, out);
    float w = p.weight * amount;
    const float front[] = {.32f, .38f, .50f}, back[] = {.07f, .085f, .12f},
                ambient[] = {.075f, .09f, .12f};
    for (int i = 0; i < 3; i++) {
        out[i] = p.direction[i];
        out[4 + i] = std::clamp(out[4 + i] + front[i] * w, 0.f, 1.f);
        out[8 + i] = std::clamp(out[8 + i] + back[i] * w, 0.f, 1.f);
        out[20 + i] = std::clamp(out[20 + i] + ambient[i] * w, 0.f, 1.f);
    }
}
inline bool cameraProjection(float vp[16], const float wvp[16], const float world[12]) {
    float a = world[0], b = world[1], c = world[2], d = world[4], e = world[5], f = world[6],
          g = world[8], h = world[9], i = world[10];
    float det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12f)
        return false;
    float inv[16] = {(e * i - f * h) / det,
                     (c * h - b * i) / det,
                     (b * f - c * e) / det,
                     0,
                     (f * g - d * i) / det,
                     (a * i - c * g) / det,
                     (c * d - a * f) / det,
                     0,
                     (d * h - e * g) / det,
                     (b * g - a * h) / det,
                     (a * e - b * d) / det,
                     0,
                     0,
                     0,
                     0,
                     1};
    for (int r = 0; r < 3; r++)
        inv[r * 4 + 3] =
            -inv[r * 4] * world[3] - inv[r * 4 + 1] * world[7] - inv[r * 4 + 2] * world[11];
    for (int r = 0; r < 4; r++)
        for (int c2 = 0; c2 < 4; c2++) {
            vp[r * 4 + c2] = 0;
            for (int k = 0; k < 4; k++)
                vp[r * 4 + c2] += wvp[r * 4 + k] * inv[k * 4 + c2];
            if (!std::isfinite(vp[r * 4 + c2]))
                return false;
        }
    return true;
}
inline bool disk(float out[4], const float vp[16], Phase p, unsigned width, unsigned height) {
    float clip[4]{};
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++)
            clip[r] += vp[r * 4 + c] * p.direction[c];
    if (!std::isfinite(clip[3]) || clip[3] <= .001f || !width || !height)
        return false;
    float x = clip[0] / clip[3], y = clip[1] / clip[3];
    if (!std::isfinite(x) || !std::isfinite(y) || std::abs(x) > 1.1f || std::abs(y) > 1.1f)
        return false;
    out[0] = (x * .5f + .5f) * width;
    out[1] = (-y * .5f + .5f) * height;
    out[2] = height * .018f;
    out[3] = p.weight * .75f;
    return p.weight > 0;
}
} // namespace alphaMoon
