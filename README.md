# Barony VR — Public Beta 2

An unofficial native OpenXR PC VR mod for **Barony**, maintained by Nyxaroth37. A purchased Steam copy of Barony is required. This project is not affiliated with or endorsed by Turning Wheel LLC.

## Download and play

Download **BaronyVR_GA21_PublicBeta2_Windows.zip** from [Releases](https://github.com/Nyxaroth37/BaronyVR/releases). GitHub's automatic **Source code** downloads are for developers and do not contain the playable runtime.

1. Connect your headset to your Windows PC and start Steam and your OpenXR runtime.
2. Extract the entire ZIP. Double-click **Setup.cmd**.
3. Confirm the detected retail Barony folder and select **Install beta**.
4. Select **Play in VR**, or use the **Barony VR - GA21 Beta** desktop/Start menu shortcut. The shortcut runs **BaronyVR.exe**.

No Git, Visual Studio, or compilation is required for players. The package is unsigned. If Windows blocks it, verify its origin and the release checksum before deciding whether to run it; do not disable system protection.

## Compatibility

| Requirement | Public Beta 2 |
| --- | --- |
| Operating system | Windows x64 |
| Retail game | Steam Barony build **22630456**; other builds are blocked |
| Verified headset/runtime | **Quest 3 with SteamVR/OpenXR** |
| Other headsets | Untested; compatibility and controller mappings are not promised |
| Quest standalone | Not supported; a Windows PC VR connection is required |
| Other stores | Epic/GOG builds are not supported by this release |
| Dependencies | Active 64-bit OpenXR runtime and Microsoft Visual C++ x64 runtime |

## Included VR features

- Tracked head and hands, VR melee interactions and shield blocking.
- Wrist HP/mana and minimap, belt HUD, companion health displays, and skill notifications.
- Equipment, bow, body, and HUD calibration controls.
- Physical shortbow draw/release with selectable draw hand. Right draw hand holds the bow in the left hand, and vice versa.
- Right thumb-rest touch plus left stick for cardinal D-pad input on compatible controllers. Lift your thumb to return to movement.
- Native bumper interactions retained outside other VR input ownership; Quick Turn uses right-stick down rather than right grip.

These are the accepted GA21 development features, not a claim that every class, item, controller, or multiplayer combination has been tested. Physical shortbow handling is for singleplayer/host. Multiplayer clients retain the native fallback; full VR bow aiming synchronization is not implemented. Bow offhand mesh presentation remains limited.

## Calibration and saves

The five bundled VR settings files are ResidentMonster's saved defaults, including seated mode and a right draw hand. Adjust body proportions, equipment alignment, HUD position, and handedness for yourself using calibration.

Setup and launch copy a default file **only when that file is missing**. Existing calibration and player edits are preserved. This beta shares the retail game's normal configuration and saves. Back up important saves before beta testing.

The fixed launcher installs under `%LOCALAPPDATA%\BaronyVR\Managed\Launcher`; separate runtime versions are under `%LOCALAPPDATA%\BaronyVR\Managed\Versions`. The retail EXE, DLLs, and artwork are not replaced. The launcher uses the retail directory as its working directory and loads Steam/FMOD from the player's installation. Calibration, ordinary settings, saves, and logs use the retail directory, which must be writable by the player.

## Updates and removal

The launcher checks published GitHub releases without requiring a GitHub account. **Update and play** downloads and verifies a compatible beta package, installs it separately, and switches versions only after verification. **Play without updating** works while checks run or when offline. Source commits alone do not update players. The beta channel includes compatible prereleases.

Players upgrading from Public Beta 1 must download and run the Public Beta 2 installer once to get the updater. Existing calibration and saves are preserved. Future releases must include compatible update metadata; see [Publishing updates](docs/UPDATING.md). Releases requiring a newer launcher offer the full installer instead.

**Use previous version** switches to the retained rollback runtime. Cleanup keeps the active and previous runtime, removes successful temporary work, and limits abandoned work. It never targets retail files, saves, calibration, or development backups. Unknown files or linked folders stop cleanup of that folder. The verified older Beta 1 install and prior launcher backup are removed only after a successful game exit with a rollback version retained.

To remove the managed installation, close Barony and its launcher, remove the desktop/Start menu shortcut, then delete `%LOCALAPPDATA%\BaronyVR\Managed`. Keep retail calibration files if you want to retain settings. Vanilla Barony remains installed.

## Known limits and bug reports

First-run story presentation and skipping have not been fully validated in the headset. Controller profiles beyond Oculus Touch and headsets beyond the tested Quest 3 setup need testing. Broader multiplayer testing is still needed. This is a beta, not a universal headset compatibility release.

Report problems through [Issues](https://github.com/Nyxaroth37/BaronyVR/issues). Include retail build, headset, connection software, active OpenXR runtime, reproduction steps, and whether the issue also occurs in vanilla Barony. `log.txt` and `crashlogs` are in your retail folder. Review logs for personal information before sharing. Do not upload retail artwork, SDKs, or game binaries.

## Source and attribution

The source is a clean snapshot of accepted GA21 commit `803239368ebbf726f8669a506892089391d0a540` from the private development repository. Earlier history, development databases, private notes, and retail language/font data are not included. The seven accepted VR/native source files retain their exact baseline bytes; see `docs/GA21-PROVENANCE.json`.

Installer and C# launcher source are under `packaging/GA21-PublicBeta2`. The release asset contains the tested prebuilt runtime; runtime binaries are not committed to this repository. Building the game yourself requires a C++17 Windows toolchain, its dependencies, and separately obtained SDKs where enabled. The inherited CMake configuration still contains a machine-specific vcpkg GLEW library path; this source snapshot is not a turnkey reproducible build setup. Do not redistribute proprietary SDKs.

Barony source copyright belongs to Turning Wheel LLC and its contributors. See [LICENSE.txt](LICENSE.txt), the accompanying component license files, and the release's `Notices` directory. The upstream README is retained as [UPSTREAM_README.md](UPSTREAM_README.md). No retail game assets or proprietary runtime DLLs are included in the beta download.
