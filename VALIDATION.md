# Validation

Version 0.3.0 leaf-motion replay and cloud-shadow prototype, 2026-10-11.

Packaged AlphaShadows.dll SHA-256: b542b35ec690cb4647d14422d580eef1b2212e56e33473d605195457a7c345d5

MSVC x86 optimized build and hidden HAL D3D9 checks pass: prior filtering, normals, terrain cache, local lights, moon lighting/occlusion and resets; new cloud receiver attenuation, actual caster depth, footprint culling, 16-draw ceiling and immediate mask reuse. Motion GPU tests confirm that explicit replay matches visible leaf motion exactly. The cloud map is released on reset with the terrain/moon resources.

In-game combined FPS, leaf-shadow alignment and cloud-shadow appearance remain unverified. Nighttime moon appearance remains unverified.
