## BaronyVR 1.0.0 — First Public Release

The first full public release of BaronyVR: accepted GA21 native OpenXR gameplay with an update-enabled launcher.

### Install

Download **BaronyVR_1.0.0_Windows.zip**, extract it completely, and run **Setup.cmd**. Confirm the retail Barony folder and select **Install launcher**. Use the **BaronyVR** desktop/Start menu shortcut afterward; it launches **BaronyVR.exe**.

Earlier prerelease users must run this installer once to get the corrected updater. Existing calibration and saves are preserved. Default calibration credit: **ResidentMonster**.

### Launcher

- Checks published stable releases without a GitHub login. Drafts, prereleases, and source-only commits do not update players.
- Offers **Update and play** and **Play without updating**, including offline play.
- Verifies GitHub asset digests and package file hashes before activation.
- Retains the prior runtime for rollback.
- Keeps two managed runtime versions and cleans completed/abandoned updater work within bounded retention.
- Migrates the recognized old beta shortcut to BaronyVR and reuses the existing Barony icon.
- Preserves retail EXE/DLLs, artwork, calibration, settings, and saves.

This build fixes Windows PowerShell's non-enumerated release-array handling. The failed Beta 2 publication remains a draft; its history/tag were not rewritten. Public version **1.0.0** uses internal update sequence **3** and update protocol **1**.

### Compatibility

Windows x64, Steam Barony build **22630456**, an active 64-bit OpenXR runtime, and the Microsoft Visual C++ x64 runtime are required. Quest 3 with SteamVR/OpenXR is the verified development setup. Other headset/controller combinations are unverified. This is PC VR, not a standalone Quest app. The package is unsigned.

Existing GA21 physical shortbow, multiplayer, offhand presentation, and first-run story limitations are unchanged. See the repository README for details.

### Validation

Windows updater tests cover verification failures, rollback/retention, unsafe archives, owned shortcut-backup cleanup, offline state preservation, and the hidden-window message loop. Gameplay and normal exit were tested; exit code 0, two retained runtimes, zero temporary folders, and removal of obsolete installer/launcher folders were verified. This publishing kit additionally tests the exact REST array response shape, stable-only updates, shortcut migration, and the actual public download/apply/rollback path.
