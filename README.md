# BaronyVR 1.0.0 — First Public Release

Native OpenXR PC VR for **Barony**, created by **ResidentMonster** (Nyxaroth37). This is an unofficial mod, not affiliated with or endorsed by Turning Wheel LLC. A purchased Steam copy of Barony is required.

## Download and play

Download **BaronyVR_1.0.0_Windows.zip** from [Releases](https://github.com/Nyxaroth37/BaronyVR/releases/tag/v1.0.0). GitHub's automatic Source code archives are for developers and do not contain the playable runtime.

1. Connect your headset to your Windows PC and start Steam and your active OpenXR runtime.
2. Extract the entire ZIP and double-click **Setup.cmd**.
3. Confirm the detected retail Barony folder and select **Install launcher**.
4. Use **Open launcher** or the **BaronyVR** desktop/Start menu shortcut.
5. Select **Play without updating**, or **Update and play** when a newer release is available.

No Git, Visual Studio, source download, or compilation is required for players. The package is unsigned. Verify its origin and release checksum before deciding whether to run it; do not disable system protection.

## Compatibility

| Requirement | Version 1.0.0 |
| --- | --- |
| Operating system | Windows x64 |
| Retail game | Steam Barony build **22630456**; other builds are blocked |
| Verified headset/runtime | **Quest 3 with SteamVR/OpenXR** |
| Other headsets/controllers | Untested; compatibility and mappings are not promised |
| Quest standalone | Not supported; a Windows PC VR connection is required |
| Other stores | Epic/GOG builds are not supported by this release |
| Dependencies | Active 64-bit OpenXR runtime and Microsoft Visual C++ x64 runtime |

## VR features

- Tracked head and hands, VR melee interactions, and shield blocking.
- Wrist HP/mana and minimap, belt HUD, companion health displays, and skill notifications.
- Equipment, bow, body, and HUD calibration.
- Physical shortbow draw/release with selectable draw hand. A right draw hand holds the bow in the left hand, and vice versa.
- Right thumb-rest touch plus left stick for cardinal D-pad input on compatible controllers. Lift your thumb to return to movement.
- Native bumper interactions retained outside other VR input ownership; Quick Turn uses right-stick down rather than right grip.

Gameplay uses the accepted **GA21** baseline. Not every class, item, controller, or multiplayer combination has been tested. Physical shortbow handling is for singleplayer/host; clients retain the native fallback. Full VR bow aiming synchronization is not implemented. Bow offhand mesh presentation remains limited.

## Calibration and saves

The five bundled VR files are **ResidentMonster's saved defaults**, including seated mode and a right draw hand. Adjust proportions, equipment alignment, HUD placement, and handedness for yourself using calibration.

Setup and launch create a default file **only when that file is missing**. Existing calibration and player edits are preserved. This release shares the retail game's ordinary configuration and saves. Back up important saves before trying a mod.

The launcher installs under `%LOCALAPPDATA%\BaronyVR\Managed\Launcher`. Runtime versions live under `%LOCALAPPDATA%\BaronyVR\Managed\Versions`. The retail EXE, DLLs, and artwork are not replaced. Steam and FMOD load from your own retail installation. Retail Barony must remain installed, and its directory must be writable for calibration, settings, saves, and logs.

## Updates, rollback, and cleanup

The launcher checks **published stable GitHub releases**, without requiring a GitHub login. Drafts and prereleases are skipped. Pushing a source commit does not update players.

**Update and play** verifies the downloaded package, installs it separately, and switches versions after validation. **Play without updating** remains available while checks run or when offline. **Use previous version** switches to the retained rollback runtime without resetting calibration or saves.

Users of earlier prereleases need to run the 1.0.0 installer once to get the corrected updater. The recognized old beta shortcut is replaced with **BaronyVR**, rather than leaving duplicate shortcuts.

Cleanup keeps the active and one previous managed runtime. Successful temporary work is removed; abandoned updater work is bounded and ages out. Saves, calibration, retail files, and development backups are never cleanup targets. Unexpected files and linked folders block deletion of that folder. Verified legacy installer and launcher folders are removed only with a rollback version retained and after a successful game exit.

To remove the mod, close Barony and the launcher, remove the BaronyVR shortcuts, and delete `%LOCALAPPDATA%\BaronyVR\Managed`. Keep retail calibration files to retain your settings. Vanilla Barony remains installed.

## Known limits and reports

First-run story presentation/skipping in the headset and broader multiplayer combinations still need testing. Additional headset/controller profiles need verification. The version number does not imply universal headset support.

Report issues through [Issues](https://github.com/Nyxaroth37/BaronyVR/issues). Include your retail build, headset, connection software, active OpenXR runtime, reproduction steps, and whether vanilla Barony is affected. `log.txt` and `crashlogs` are in retail Barony. Review logs for personal information before sharing; do not upload retail artwork, SDKs, or game binaries.

## Source and attribution

Game source derives from accepted GA21 commit `803239368ebbf726f8669a506892089391d0a540`; see [provenance](docs/GA21-PROVENANCE.json). The original development history remains private. Installer, updater, and C# launcher source are under `packaging/v1.0.0`. Runtime binaries are distributed as release assets rather than Git source files.

Developers: see [publishing updates](docs/UPDATING.md). Building the game requires a C++17 Windows toolchain, dependencies, and separately obtained SDKs where enabled. The inherited CMake configuration still has a machine-specific vcpkg GLEW library path; the source snapshot is not a turnkey reproducible build setup.

Barony source copyright belongs to Turning Wheel LLC and its contributors. See [LICENSE.txt](LICENSE.txt), component license files, and the release's Notices directory. [UPSTREAM_README.md](UPSTREAM_README.md) preserves the upstream announcement. The shortcut reuses Barony's existing icon; no new visual artwork was created. Retail game assets and proprietary retail runtime DLLs are not included in the download.
