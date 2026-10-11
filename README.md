# AlphaCWShadows

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
versions may not work. Steam-only moon and SDK world-state features are not
included. Residual distant banding and shimmer remain under investigation.

Run `build\tests\DirectionalGpuTest.exe`. Hardware GPU tests passed locally
for filtering, six normal IDs, small-entity external shadows, local lighting,
warmth, and terrain origin shifts. User testing confirmed improved visuals and
Alt+Tab stability with the paired TAA build. These are not universal guarantees.
