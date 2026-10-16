# Validation

2026-10-16 Steam-fix port. Built with MSVC 14.44 x86 C++17 `/O2 /MT /EHsc`.

AlphaShadows.dll SHA-256: `0c4011e72ecbcb11cbb7c2f1f4026f9f0d285f548675b71ce1f6056c3a83bf1c`

`DirectionalGpuTest`, `MoonRulesTest` and `CampfireLightTest` pass on a
hardware D3D9 device. All previous checks (filtering, six normal IDs, focused
and far maps, small entities, local lights, warmth, clouds, terrain cache,
moon lighting and occlusion, resets) still pass with `DirectLight=0` and give
identical values. New checks:

- Direct-light model: a fully direct-lit ground shadow is unchanged (102 of
  255 at strength 0.6), with 0.3 ambient it keeps that light (140), next to a
  lantern it disappears as natively (255), and faces turned away from the sun
  are not darkened again (255).
- Terrain face grazed at 8 degrees: occluded and open texels shade alike
  (207/207); a model face at the same angle still resolves its occluder.
- Camera fade: a camera 90 blocks up with Radius 100 lightens the shadow
  (201 vs 102); beyond the radius nothing is drawn.
- Moon rules: horizon window (none below z = 0.08, full from 0.30) and a dusk
  light turn of at most 2.65 degrees per game minute.

The native lighting formula comes from disassembling Alpha's vs_3_0 world
shader in Cube.exe (SHA-256 84a7a132...). In-game appearance and frame time
with this build are not yet verified.
