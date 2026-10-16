# AlphaCWShadows

Version 0.3 adds explicit moving-leaf shadow replay and bounded cloud shadows.
Version 0.2 added a visible procedural moon, cooler/brighter native night lighting,
and moon-cast shadows using the existing directional map. Made with AI assistance
from OpenAI ChatGPT/Codex, adapting Steam MoonandShadow's MIT-licensed moon rules.

Alpha's native sun direction drives the opposite moon direction, twilight fade,
and shadow cycle; this mod never changes the game clock. Native front/back and
ambient colors receive the same blue lighting additions as the Steam approach.
The moon is a pixel-styled disk with procedural craters, depth-occluded by terrain,
characters, and clouds. It is drawn before the HUD and the paired TAA resolve.

In `graphics-rebuild/AlphaDirectional.ini`, `[Moon]` provides `Enabled=1`,
`VisibleMoon=1`, `BrightnessPercent=4` (0–50), and `ShadowStrengthPercent=60`
(0–100). Defaults match the inspected Steam moon settings. F7 toggles the whole
shadow/moon setup. The normal sun shadows and warmth remain active by day.

`build\tests\MoonRulesTest.exe` checks phase, blue lighting, protected native
constants, camera reconstruction, and moon projection. The GPU test also checks
moon occlusion, draw-state restoration, native lighting restoration, and three
device resets. These passed locally on 2026-10-11; nighttime appearance still
needs in-game verification. No additional night shadow map or terrain-lighting
replay is introduced; the visible disk adds one small quad at night.

Real-time 4K directional voxel shadows for Cube World Alpha.

Made by DieuDeGlace with AI assistance from OpenAI ChatGPT/Codex. MIT licensed. No game executable, game assets, saves, or runtime journals are included.

## Build

Install Visual Studio 2022 Build Tools with Desktop development with C++ and
the Windows SDK. Open an **x86 Native Tools Command Prompt for VS 2022**.
These Alpha DLLs are 32-bit; an x64 build will not load into Cube.exe.

```bat
git clone https://github.com/DieuDeGlace/AlphaCWShadows.git
cd AlphaCWShadows
build.cmd
```

The DLL appears under `build/Mods/`. No toolchain or game files are bundled.
Builds use C++17, /O2 /MT /EHsc, Unicode, and the Windows SDK. clang-cl in an
x86 MSVC environment may be substituted for cl. Test programs are built into
`build/tests/`; run them with Alpha and other exclusive-fullscreen games closed.
GPU tests require a working Windows D3D9 driver and do not measure in-game FPS.

## Install

Close Alpha, extract the release ZIP into the Alpha game folder, and launch
through the Alpha mod loader. The ZIP's `Mods` folder must merge with the
game's existing `Mods` folder. Keep the included configuration/support folders.
The repository name is the project name; the established DLL filename stays
unchanged for compatibility. Remove the DLL with the game closed to uninstall.

## Shadows and controls

4096x4096 R32F directional map with narrow filtered comparisons, native voxel
face decoding, terrain caching, local-light protection, and warmer sunlight.
Small models use the accepted Steam shadow method's face-normal footprint and
minimum depth tolerance, avoiding derivatives that cross adjacent voxel faces.
Receiver projection shares CWSMTAA jitter when its AlphaTAA.dll is installed.

`graphics-rebuild/AlphaDirectional.ini` controls resolution, radius, strength,
bias, warmth, and filtering. F7 toggles shadows. Windows d3dcompiler_47.dll is
required for runtime shader compilation. No additional SDK is required to build.
Native mesh discovery targets the inspected 32-bit Alpha layout; other game
versions may not work. Steam SDK world-state features are not included. Residual distant banding and shimmer remain under investigation.

Run `build\tests\DirectionalGpuTest.exe`. Hardware GPU tests passed locally
for filtering, six normal IDs, small-entity external shadows, local lighting,
warmth, and terrain origin shifts. User testing confirmed improved visuals and
Alt+Tab stability with the paired TAA build. These are not universal guarantees.

## Source formatting

The project uses the checked-in `.clang-format` style (four-space indentation and a 100-column target). From the repository root, run `clang-format -i src/*.cpp src/*.h` on the source files that exist. Keep third-party SMAA files and embedded shader strings unchanged.

The cleanup was rebuilt with MSVC for x86 and checked against the previous revision for identical executable tokens and shader strings. Runtime settings and installed DLLs are unchanged.

## Moving leaves and cloud shadows

Install AlphaCWWorldMotion 0.2 alongside this version to bind shadows to its leaf motion. Shadow replay explicitly calls the motion function rather than relying on the normal draw hook, preserving the same corner weights, displacement and range fade for visible, caster and receiver draws. Unanimated shadow draws use a fast path.

Clouds use a separate 512px R32F/D24S8 map with a 4096-block depth span; the existing 4K terrain map and its 256-block depth span are unchanged. Only cloud bounds intersecting the nearby shadow footprint are admitted, nearest first, with at most 16 caster submissions per update. The mask normally updates at most 10 times per second; a large origin jump can trigger a necessary immediate refresh. Empty candidate sets add no map pass. Four comparisons are blended into the existing receiver shader, so there is no extra world receiver pass. The map uses about 2 MiB of texture/depth memory.

`[Clouds] Enabled=1` and `ShadowStrengthPercent=24` (0-50) in `graphics-rebuild/AlphaDirectional.ini` control the effect. A short grace period preserves the previous mask when clouds are temporarily camera-culled; persistent native culling can still remove cloud coverage. Movement between limited-rate updates is small but not temporally interpolated. Native cloud transforms are captured after drift and are not translated again during replay.

The user reported good performance for the original motion-only mod. Combined live FPS and cloud/leaf shadow appearance still need verification. Hidden tests verify explicit motion replay, cloud caster depth, footprint culling, draw ceiling, mask reuse, receiver attenuation/local-light protection and resource cleanup.

## Steam MoonandShadow fixes (2026-10-16)

Ported from the Steam version's 2026-10-13 shadow updates. Alpha already casts
daytime shadows from the game's own light direction (the Steam `SunFromGame`
fix), so that one needed no change.

- **As dark as unlit faces** (`DirectLight=1`, default). The receiver now
  evaluates Alpha's own world vertex lighting (disassembled from the shipped
  vs_3_0 shader: ambient, front and back light, material alpha, point lights,
  the 1.2 clamp and the darkness color) and a shadow removes only the direct
  sunlight share. At `Strength=1.0` (new default) a shadowed surface matches a
  face turned away from the sun in the unmodified game; ambient, back and
  lantern light stay, and faces already turned away are not darkened twice.
  `DirectLight=0` restores the previous uniform darkening.
- **Dusk and dawn without hard switches.** Shadows reach zero within about 4.6
  degrees of the horizon and full strength from about 17.5 degrees, for the sun
  and the moon alike, and the shadow passes are skipped in between. At night
  the native light direction now turns toward the moon as it fades in (passing
  overhead), instead of jumping at sunset, so block faces no longer flip
  between lit and unlit.
- **Fade by distance from the camera** (`CameraFade=1`, default), like Omega:
  zooming out shortens the shadow reach. `FadeStartPercent=75` keeps the
  previous fade shape. Pixels beyond the fade skip the shadow-map lookups.
- **No noon banding on cliff sides.** Terrain faces the sun grazes at under
  about 9 degrees get one consistent shade (fading to normal sampling by 17
  degrees), with a slope-scaled base bias just past that band. Models keep the
  previous edge-on fade.

The Steam version's draw-batching, inline-terrain and buffer-layout changes are
D3D11-specific and are not part of this D3D9 port.

`DirectionalGpuTest` covers the direct-light model (all-direct, ambient kept,
lantern, faces turned away), grazing terrain without stripes and the camera
fade; `MoonRulesTest` covers the horizon window and a light turn of at most
2.65 degrees per game minute through dusk. In-game appearance is not yet
verified.
