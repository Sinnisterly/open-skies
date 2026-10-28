# Open Skies

Drop-in No Man's Sky mod menu. You put one file (`version.dll`) next to `NMS.exe` and you're done. No loader needed. Offline singleplayer only.

- **Version:** 0.9.0
- **Game:** No Man's Sky, Steam build 180836, Windows, Vulkan
- **Open menu:** F8 (rebindable in Settings). Q / E switch tabs.

## Unofficial Disclaimer
This is an unofficial modification for No Man's Sky. It is not approved, authorized,
supported, or affiliated with Hello Games in any way.

Use it at your own risk. The authors cannot guarantee it keeps working after game updates.
Hook targets are found by pattern at startup, so a feature whose target moved shows as
Unavailable instead of breaking the rest.

This mod is and will always remain free to use and download.

## Support me on Ko-Fi if you want!
- https://ko-fi.com/loathed
- Any donation is appreciated, but none are needed; I was asked to add this by users, that is all.

---

## What you get

A full overlay menu: survival and movement cheats, an item spawner, unlocks, expeditions, a Garage for ships, Multi-Tools and characters (parts, paint, seeds, Add to Garage), storage transfers from any distance, a system scout, teleport and warp to any galaxy, building tools, effects and custom weather, a tuner, and hotkey binds.

Settings, teleport spots, placed effects and a log land next to the game: `OpenSkies.settings.json`, `OpenSkies.bookmarks.json`, `OpenSkies.effects.json`, `OpenSkies.log`. If that folder isn't writable, they go under `%AppData%\OpenSkies\` instead. A save backup is made in `OpenSkies.backups\` before the first save change of each session.

See [CHANGELOG.md](CHANGELOG.md) for every version.

---

## Install

1. Build `version.dll` (below) or grab one from Releases.
2. Copy it into the game's `Binaries` folder, next to `NMS.exe`, or:

```powershell
.\Install-OpenSkies.ps1 -Game "D:\SteamLibrary\steamapps\common\No Man's Sky"
```

3. Launch No Man's Sky and hit **F8**.

To uninstall, delete `version.dll` or run `Uninstall-OpenSkies.ps1 -Game "..."`. Optional leftovers: the `OpenSkies.*` files and folders.

---

## Branches

| Branch | What's on it |
|--------|--------------|
| `main` | Default. Normal credits with the logo. |
| `forum` | Same code; in-menu credit text is `by loathecsgo`, no logo. |

New work goes on `main`. `forum` is `main` plus one commit that changes `ui.credit` and `ui.credit.logo` in `locales/en.json`.

---

## Build

You'll need Windows x64, Visual Studio 2022 with the C++ desktop workload, and CMake 3.24+ (the one bundled with Visual Studio works).

```powershell
git clone --recursive https://github.com/Sinnisterly/open-skies.git
cd open-skies
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Outputs `build\Release\version.dll` and `version.pdb`. The first configure downloads Zydis for SafetyHook.

Dependencies are git submodules in `external/`: Dear ImGui 1.92.9b, SafetyHook 0.6.10, nlohmann/json 3.12.0, Vulkan-Headers 1.4.350 and zstd 1.5.7. If you cloned without `--recursive`, run `git submodule update --init`.

```
src/core/      settings, hotkeys, locale, feature catalog
src/assets/    game pak reader and font loading
src/ui/        theme, widgets, menu
src/render/    Vulkan hooks, overlay, textures, model preview
src/input/     window subclass and input blocking
src/hooks/     image scanner and target locator
src/game/      game thread, item tables, prices, save backup
src/features/  every tab and feature
src/mod/       startup
proxy/         version.dll exports forwarded to System32
game/          hook targets and expedition data, built into the DLL
locales/       English strings, built into the DLL
```

---

## Localization

Strings live in `locales/en.json`, which is built into the DLL. To add a language, copy it to `Locales\xx.json` beside the game (or under `%AppData%\OpenSkies\`), translate the values, and pick it in Settings. Missing keys fall back to English.

- Change values, not keys
- Keep `{0}` / `{1}` in the same order as English

---

## Credits

Feature research builds on the No Man's Sky Cheat Engine table by BD, with scripts by BigJit901, gir489 and lucar. Expedition season data comes from the cwmonkey/nms-expeditions archive (original files by BorisDeLeodium). Expedition exclusives list from Nexus mod 2589.

## License

GPL-3.0. See [LICENSE](LICENSE).
