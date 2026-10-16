#pragma once
#include <cstdint>
#include <cstring>

namespace alphaCampfire {
constexpr unsigned bridgeSize = 34;
inline void jump(unsigned char *out, uintptr_t instruction, uintptr_t target) {
    out[0] = 0xe9;
    uint32_t relative = uint32_t(target - instruction - 5);
    memcpy(out + 1, &relative, 4);
}
// The native loop has already selected campfire (65) or torch (51) scenery.
// Preserve its input registers/flags and bypass daylight attenuation only for 65.
inline void bridge(unsigned char *out, uintptr_t address, uintptr_t resume) {
    const unsigned char code[bridgeSize] = {
        0x9c, 0x50,                                        // pushfd; push eax
        0x8b, 0x44, 0x24, 0x1c,                            // original scenery pointer at esp+14
        0x83, 0x38, 0x41, 0x74, 0x10,                      // cmp kind,65; je campfire
        0x58, 0x9d,                                        // restore eax/flags
        0xf3, 0x0f, 0x5c, 0x84, 0x24, 0xc4, 0,    0,    0, // original subss
        0xe9, 0,    0,    0,    0,    0x58, 0x9d, 0xe9, 0, 0, 0, 0};
    memcpy(out, code, sizeof(code));
    jump(out + 22, address + 22, resume);
    jump(out + 29, address + 29, resume);
}
} // namespace alphaCampfire
