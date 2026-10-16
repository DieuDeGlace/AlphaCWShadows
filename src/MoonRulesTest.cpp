#include "AlphaMoonRules.h"
#include <cassert>
#include <cstdio>
int main() {
    float day[3] = {0, 0, 1}, night[3] = {0, 0, -1}, horizon[3] = {0, 1, 0};
    auto d = alphaMoon::phase(day, .6f), n = alphaMoon::phase(night, .6f),
         h = alphaMoon::phase(horizon, .6f);
    assert(d.weight == 0 && d.strength == 0 && h.weight == 0);
    assert(n.weight == 1 && n.strength == .6f && n.direction[2] == 1);
    float native[24]{}, changed[24]{};
    native[0] = 0;
    native[1] = 0;
    native[2] = -1;
    native[3] = 123;
    native[12] = 4;
    native[16] = .9f;
    native[23] = 1;
    alphaMoon::lighting(changed, native, n, .04f);
    assert(changed[6] > changed[5] && changed[5] > changed[4]);
    assert(changed[22] > changed[21] && changed[21] > changed[20]);
    assert(changed[3] == 123 && changed[12] == 4 && changed[16] == .9f && changed[23] == 1);
    float world[12] = {2, 0, 0, 3, 0, 4, 0, 5, 0, 0, 6, 7};
    float wvp[16] = {2, 0, 0, 3, 0, 4, 0, 5, 0, 0, 6, 7, 0, 0, 0, 1}, vp[16]{};
    assert(alphaMoon::cameraProjection(vp, wvp, world));
    for (int i = 0; i < 16; i++)
        assert(std::abs(vp[i] - (i % 5 == 0 ? 1.f : 0.f)) < 1e-5f);
    world[0] = 0;
    assert(!alphaMoon::cameraProjection(vp, wvp, world));
    float projection[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 1, 0};
    float disk[4]{};
    assert(alphaMoon::disk(disk, projection, n, 1920, 1080));
    assert(disk[0] == 960 && disk[1] == 540 && disk[2] > 19 && disk[2] < 20);
    auto behind = n;
    behind.direction[2] = -1;
    assert(!alphaMoon::disk(disk, projection, behind, 1920, 1080));
    // Steam port: no shadows within ~4.6 degrees of the horizon, full above ~17.5.
    assert(alphaMoon::horizonFade(.079f) == 0 && alphaMoon::horizonFade(0) == 0);
    assert(alphaMoon::horizonFade(.30f) == 1 && alphaMoon::horizonFade(1) == 1);
    assert(alphaMoon::horizonFade(.15f) > 0 && alphaMoon::horizonFade(.15f) < .5f);
    // The native light turns toward the moon gradually through dusk (passing
    // overhead for opposite directions) instead of jumping at sunset.
    float previous[3] = {0, 0, 0};
    float worstStep = 0;
    for (int step = 0; step <= 400; step++) {
        float angle = -.05f - step * .0025f; // Sun from just below the horizon to deep night.
        float sunDirection[3] = {std::cos(angle), 0, std::sin(angle)};
        auto dusk = alphaMoon::phase(sunDirection, .6f);
        float nativeLights[24]{}, out[24]{};
        std::copy(sunDirection, sunDirection + 3, nativeLights);
        if (dusk.weight > 0)
            alphaMoon::lighting(out, nativeLights, dusk, .04f);
        else
            std::copy(nativeLights, nativeLights + 24, out);
        float length = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
        assert(std::abs(length - 1) < 1e-3f);
        if (step) {
            float d = std::clamp(out[0] * previous[0] + out[1] * previous[1] + out[2] * previous[2],
                                 -1.f, 1.f);
            worstStep = std::max(worstStep, std::acos(d) * 57.2958f);
        }
        std::copy(out, out + 3, previous);
    }
    // Each step is 0.0025 rad of sun travel, about 0.57 game minutes.
    float perMinute = worstStep / .573f;
    assert(perMinute < 3);
    float setting[3] = {1, 0, 0}, rising[3] = {-1, 0, 0}, middle[3]{};
    alphaMoon::blendDirection(middle, setting, rising, .5f);
    assert(std::abs(middle[2] - 1) < 1e-4f); // Opposite horizons meet overhead.
    printf("Gradual dusk light turn: at most %.2f degrees per game minute (Steam limit 3)\n",
           perMinute);
    puts(
        "PASS: native phase, twilight, moon shadows, blue lighting, protected constants, camera reconstruction, disk projection, horizon fade and gradual light turn.");
}
