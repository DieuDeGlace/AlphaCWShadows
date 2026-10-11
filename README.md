# AlphaCWShadows

Version 0.2 adds a visible procedural moon, cooler/brighter native night lighting,
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
