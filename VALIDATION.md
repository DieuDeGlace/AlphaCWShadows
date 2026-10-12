# Validation

2026-10-12 update. Built MSVC x86 C++17 `/O2 /MT /EHsc`.

DLL: `AlphaShadows.dll`

SHA-256: `dd01b2742562512fb04f25678f51dcdff9379e1bdeb9da0769df7ad6c6b459a8`

Actual x86 CampfireLightTest and MoonRulesTest pass. The hidden D3D9 DirectionalGpuTest passes near/far cascades, native local lights, all six face normals, cloud budget, moon state restoration, and reset/resource checks. Native executable instruction signatures match the supported installed Alpha build. Live daytime appearance is not yet verified.
