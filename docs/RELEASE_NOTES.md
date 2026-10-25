## Barony VR — GA21 Public Beta 1

Unofficial native OpenXR PC VR mod. **Requires Steam retail Barony build 22630456 and a Windows PC.** Verified development setup: Quest 3 with SteamVR/OpenXR. Other headsets and controller mappings are unverified; this is not a standalone Quest app.

### Install

Download **BaronyVR_GA21_PublicBeta1_Windows.zip**, extract it completely, and double-click **Setup.cmd**. Confirm your Steam Barony folder and select **Install beta**. Use **Play in VR** or the desktop/Start menu shortcut afterward. The installed launcher is **BaronyVR.exe**. No developer tools are required.

The runtime installs separately from retail Barony. It uses your own retail assets, Steam DLL, and FMOD DLL. Existing calibration is preserved; missing VR settings receive Mike's saved defaults and can be adjusted in calibration. Game settings and saves are shared with retail Barony. The package is unsigned.

### Features

Tracked head/hands, VR melee and blocking, wrist HP/mana and minimap, belt/companion/skill HUD, calibration controls, physical shortbow handling with selectable draw hand, and right thumb-rest + left-stick D-pad input on compatible controllers.

### Limits

- Physical shortbow input is for singleplayer/host; clients retain native fallback. Full VR bow aim networking is not implemented.
- Other headsets, all item/class combinations, and broader multiplayer compatibility need testing.
- Bow offhand mesh presentation and first-run story presentation/skipping remain limited or unverified.
- Only the verified retail build is accepted by this release.
- **Manual updates only.** Publishing a source commit does not update the installed game.

See the repository README for requirements, controls, removal, source provenance, and bug-report instructions. Download the Windows ZIP rather than GitHub's automatic Source code archives to play.

Validation: accepted GA21 runtime and calibration hashes checked; private Beta 1/R1 installer and BaronyVR.exe launch tested on the development headset setup; first-install defaults and repeat-install preservation tested. The public package retains the same runtime, launcher, and default file bytes with public release wording and a separate installation folder.
