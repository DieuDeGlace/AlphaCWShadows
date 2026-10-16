#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cassert>
#include <cstdio>
#include <initializer_list>
#include "CampfireLightPatch.h"

// Execute the actual bridge with a synthetic native stack, not a duplicate formula.
int main() {
    auto *code = static_cast<unsigned char *>(
        VirtualAlloc(nullptr, 128, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    assert(code);
    // cdecl float fixture(kind*, daylight*): reserve native scratch; copy arguments;
    // load xmm0=1; enter bridge; return its result through x87.
    unsigned char fixture[] = {
        0x81, 0xec, 0xd0, 0,    0,    0,    0x8b, 0x84, 0x24, 0xd4, 0,    0,    0,    0x89,
        0x44, 0x24, 0x14, 0x8b, 0x84, 0x24, 0xd8, 0,    0,    0,    0x8b, 0,    0x89, 0x84,
        0x24, 0xc4, 0,    0,    0,    0xc7, 0x44, 0x24, 0x18, 0,    0,    0x80, 0x3f, 0xf3,
        0x0f, 0x10, 0x44, 0x24, 0x18, 0xe9, 0,    0,    0,    0,    0xf3, 0x0f, 0x11, 0x44,
        0x24, 0x18, 0xd9, 0x44, 0x24, 0x18, 0x81, 0xc4, 0xd0, 0,    0,    0,    0xc3};
    memcpy(code, fixture, sizeof(fixture));
    alphaCampfire::jump(code + 47, uintptr_t(code + 47), uintptr_t(code + 80));
    alphaCampfire::bridge(code + 80, uintptr_t(code + 80), uintptr_t(code + 52));
    DWORD previous;
    assert(VirtualProtect(code, 128, PAGE_EXECUTE_READ, &previous));
    FlushInstructionCache(GetCurrentProcess(), code, 128);
    auto run = reinterpret_cast<float(__cdecl *)(int *, float *)>(code);
    for (float daylight : {0.f, .5f, 1.f}) {
        int campfire = 65, torch = 51;
        assert(run(&campfire, &daylight) == 1.f);
        assert(run(&torch, &daylight) == 1.f - daylight);
    }
    VirtualFree(code, 0, MEM_RELEASE);
    puts(
        "PASS: actual x86 campfire bridge keeps day/twilight/night emission; torch attenuation preserved");
}
