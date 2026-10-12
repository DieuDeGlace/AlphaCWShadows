# Validation

2026-10-12 update. Built MSVC x86 C++17 `/O2 /MT /EHsc`.

DLL: `AlphaShadows.dll`

SHA-256: `e744049a499abafd79083b550cd356a67d0866debe240c9221705498bc5b2b39`

Actual x86 CampfireLightTest and MoonRulesTest pass. The hidden D3D9 DirectionalGpuTest passes near/far cascades, native local lights, all six face normals, cloud budget, moon state restoration, and reset/resource checks. Native executable instruction signatures match the supported installed Alpha build. Live daytime appearance is not yet verified.

Moon-occlusion update: hidden D3D9 regression test passes an actual foreground draw with depth writes disabled after early moon composition, verifies repeated world draws do not recompose it, and retains the existing terrain occlusion/state tests. MoonRulesTest and actual x86 CampfireLightTest pass. In-game tree/model occlusion remains to be checked after restart.
