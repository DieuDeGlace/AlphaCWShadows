# AlphaCWShadows: Steam cascade test

Branch `test/steam-cascades` ports the latest Steam cascade approach to Alpha's D3D9 renderer. `main` remains the accepted single-map build. Made by DieuDeGlace with AI assistance from OpenAI ChatGPT/Codex. No proprietary code or assets are included.

Based on Steam MoonandShadow `a1ec7d9`: a focused map, blended overlap, conservative light-space caster culling, one receiver replay, redundant-binding avoidance, and inactive-light pruning. Characters and other models retain the accepted far-map receiver path, including its existing normal-derived sprite bias. Alpha's moving-leaf replay API and bounded cloud shadows remain active.

The default far map is 4096px over 120 blocks. A separately texel-snapped 2048px map covers the nearest 32 blocks, blending over its outer fifth. Receivers outside its projected bounds use the far map. The near map adds a culled caster pass and about 32 MiB of R32F plus D24/S8 memory; there is no additional receiver geometry pass. D3D9 requires world derivatives to be evaluated before cascade branching, so the implementation adapts the Steam algorithm rather than copying D3D11 API calls.

Replay bindings and constant uploads are reused when unchanged. Caster draws upload only the world transform; receiver draws upload compacted active point lights in their native order. Sparse timestamp queries log caster/receiver GPU cost where supported. Queries never flush, spin or block for results and are released before Reset. No in-game speedup or Omega parity is established by the automated checks.

## Build and install

Use Visual Studio Build Tools with Desktop development with C++ and a Windows SDK. In an **x86 Native Tools Command Prompt**, run `build.cmd`. Run `build\tests\DirectionalGpuTest.exe` and `build\tests\MoonRulesTest.exe` with exclusive fullscreen games closed. Output is `build/Mods/AlphaShadows.dll`.

Close Alpha, install this DLL in `Mods`, and place `config/AlphaDirectional.ini` in `graphics-rebuild`. Pair with CWSMTAA's `test/steam-stable-taa` branch for the full test. AlphaCWWorldMotion 0.2 keeps leaf replay and cloud drift synchronized; water/controller mods remain independent. Restart through the mod loader.

## Settings and rollback

`[Shadows] Cascades=1`, `NearResolution=2048`, `NearRadius=32`, `Resolution=4096`, `Radius=120` are the test defaults. Set `Cascades=0` and restart for a single-map comparison. F7 toggles shadows. Existing moon, blue night lighting and `[Clouds]` settings are retained; nighttime appearance still needs live verification.

The installed test's accepted DLLs, settings and main-branch source archives are preserved in `graphics-rebuild/backups/accepted-before-steam-cascades`. Restore DLLs/settings with Alpha closed for a full rollback. README and tests do not claim that live face artifacts or performance are resolved.

## Formatting

Use the checked-in `.clang-format`: four-space indentation, 100-column target. Preserve embedded HLSL strings. See `VALIDATION.md` for results. MIT licensed.

## Daytime campfire lighting

Campfires now keep their native warm point light during daylight. The native scenery-light collection still handles culling, radius, color, and placement. A signature-checked x86 bridge bypasses only the day/night attenuation for campfire scenery kind 65; torches and other lights retain their existing behavior. There are no extra draw calls, lights, shadow maps, or per-frame terrain scans. Set `[Campfires] DaytimeLight=0` in `graphics-rebuild/AlphaDirectional.ini` before launch to disable this change. The patch remains active independently of the F7 shadow toggle.

The supported native instruction site and campfire/torch selection are checked before modification. Other game threads briefly pause for the code write; installation is skipped if a thread cannot be checked or is executing the overwritten instruction. The bridge stays resident for the game session; close the game before removing or replacing the DLL.

Run `build\tests\CampfireLightTest.exe` to execute the actual x86 bridge against day, twilight, and night inputs. This update is on `test/steam-cascades`; main is unchanged. Live daytime campfire appearance and chunk-load timing still need player verification.

## Moon foreground occlusion

The moon is composed once, immediately before the first recognized main-world draw, using that frame's reconstructed camera and night phase. Trees, foliage, and character/model materials then cover it naturally, including materials that do not write depth. The HUD depth-clear hook no longer draws the moon over the completed world. Existing terrain depth occlusion remains; this adds no receiver replay, model pass, or depth texture.
