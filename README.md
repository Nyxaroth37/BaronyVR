# BaronyVR 1.0.1

Unofficial native OpenXR PC VR for **Barony**, created by **ResidentMonster** (Nyxaroth37). Retail Steam Barony is required. Not affiliated with or endorsed by Turning Wheel LLC.

## Install or update

Download **BaronyVR_1.0.1_Windows.zip** from [the 1.0.1 release](https://github.com/Nyxaroth37/BaronyVR/releases/tag/v1.0.1), extract the entire ZIP, and double-click **Setup.cmd**. Confirm the detected retail folder and install the launcher. Use the **BaronyVR** desktop/Start menu shortcut afterward. GitHub's automatic source archives do not contain a playable runtime.

Existing 1.0.0 users can select **Update and play** in **BaronyVR.exe**. Published stable releases are checked without a GitHub account. Pushing source alone does not update players. Downloads are verified, offline play is available, and **Use previous version** selects the retained rollback runtime. Fresh players need no Git, Visual Studio or compilation. The package is unsigned.

## Compatibility

| Requirement | 1.0.1 |
| --- | --- |
| OS | Windows x64 |
| Retail | Steam Barony build **22630456**; other builds blocked |
| Verified setup | Quest 3 with SteamVR/OpenXR, connected to a Windows PC |
| Other headsets/controllers | Untested; mappings/compatibility not promised |
| Standalone Quest / other stores | Not supported |
| Dependencies | Active 64-bit OpenXR and Microsoft Visual C++ x64 runtime |

## New in 1.0.1

Native-art calibration tabs and labeled sliders; selected-arm size/placement controls; separate weapon/item/crosshair and HUD pages; headset-follow belt; opt-in experimental room-scale steps and headset-relative stick movement; default bottle equipping; 50% smaller held bottles; press-at-face Grip drinking in offline single-player. See [release notes](docs/RELEASE_NOTES_1.0.1.md) for controls and limits.

Press **X+Y** for VR calibration. Body contains the room-scale tracking switch, which starts off each launch. Right thumb-rest touch plus left stick gives D-pad commands; lift your thumb for movement. Right-stick down gives Quick Turn. Physical bow draw/release uses the selected draw hand. Existing combat/blocking, wrist HP/mana/minimap, belt, companion and skill HUD paths are preserved.

For drinking, equip a nonempty potion in the normal right weapon slot, raise the RIGHT controller near your face with Grip released, then **press Grip**. Release/repress for another drink; no tilt or dwell. Keep trigger released while drinking; normal equipped trigger throwing remains available. Native potion effects and consumption apply.

## Settings and files

All five VR default files are **ResidentMonster's latest saved calibration**, including **Standing** mode and a right bow draw hand. Adjust them for your body and controllers. Setup and launch copy a default only if its file is missing. Updates preserve existing calibration and saves. Calibration, ordinary configuration and saves remain in retail Barony, whose folder must stay writable.

The launcher lives under `%LOCALAPPDATA%\BaronyVR\Managed\Launcher`; runtimes under `Managed\Versions`. The updater keeps active plus one previous managed runtime, removes verified owned temporary work, and preserves unexpected user files. Retail EXE/DLLs/artwork are not overwritten. Steam/FMOD load from the purchased retail installation. To uninstall, close the game/launcher, remove the shortcuts and Managed folder; retain retail calibration to preserve settings.

## Known limits

- Drinking and experimental room-scale/world grabbing are offline single-player pilots. Loose-object grabbing is not yet headset-confirmed. Motion-based throwing and backpack transfer are not implemented; native equipped-item throwing is available.
- A thin overhead strip in room-scale and peripheral pop-in remain unresolved. Temporary visibility tests under Controls & Help reset on restart; they do not alter headset optics.
- Physical shortbow remains single-player/host with native fallback for clients; full VR aim synchronization and some offhand presentation remain limited. First-run story display/skipping and broader multiplayer/headset/controller combinations need testing.

Report reproduction steps, retail build, headset, PC connection software and OpenXR runtime in [Issues](https://github.com/Nyxaroth37/BaronyVR/issues). Review retail `log.txt`/crashlogs for personal information before sharing. Do not upload retail assets, SDKs or game binaries in issues.

## Source and attribution

The accepted GA33 working-tree source is published here, based on private GA21 commit `803239368ebbf726f8669a506892089391d0a540`. See [1.0.1 provenance](docs/V1.0.1-PROVENANCE.json), [updating](docs/UPDATING.md), `packaging/v1.0.1` for installer source and `tests/vr-v1.0.1` for focused C++ fixtures. Runtime binaries are release assets. Building requires C++17 and separately obtained enabled SDKs/dependencies; inherited machine-specific CMake paths still need configuration.

Barony source belongs to Turning Wheel LLC and contributors. See [LICENSE.txt](LICENSE.txt), component license files, Notices and [upstream README](UPSTREAM_README.md). The existing Barony icon/artwork is reused; no new assets were generated. Proprietary retail binaries/artwork are not bundled.
