## Barony VR — GA21 Public Beta 2

**Updater and cleanup release. Accepted GA21 gameplay is unchanged.**

### Install or upgrade

Download **BaronyVR_GA21_PublicBeta2_Windows.zip**, extract it completely, and run **Setup.cmd**. Existing Public Beta 1 users need this one manual installation to receive the update-enabled launcher. After setup, use the existing desktop shortcut for **BaronyVR.exe**.

### New launcher

- Checks published GitHub beta releases without a GitHub login.
- Offers **Update and play** and **Play without updating**. Offline checks do not block play.
- Verifies GitHub asset SHA256 digests and package file hashes before switching versions.
- Retains the prior runtime for **Use previous version** rollback.
- Keeps only the active and rollback runtimes. Removes completed download/staging/check work and bounds abandoned temporary work.
- Cleans the verified older Beta 1 install and prior launcher backup after a successful game exit; unknown personal files block deletion.
- Preserves retail calibration, settings, and saves. Default profile credit is **ResidentMonster**.

Runtime versions and the fixed launcher are managed separately under `%LOCALAPPDATA%\BaronyVR\Managed`. Retail Barony is not replaced. Source commits alone do not trigger updates.

### Compatibility and limits

Windows x64 and Steam Barony build **22630456** are required. Quest 3 with SteamVR/OpenXR is the verified development setup. Other headset/controller profiles are unverified. This is PC VR, not a standalone Quest app. The package is unsigned. Existing GA21 physical shortbow, multiplayer, offhand presentation, and intro limitations are unchanged; see the repository README.

### Validation

Windows updater tests passed for download/hash rejection, payload verification, rollback, retention, unsafe ZIP paths, duplicate paths, shortcut-backup validation, update selection, offline state preservation, and the hidden-window message loop. The launcher was tested through gameplay and normal exit. The maintainer verified exit code 0, two retained runtimes, no staging/check/work folders, and removal of the obsolete Beta 1 and launcher-backup folders.

The final package uses the same tested updater/bootstrap, game executable, and default settings bytes; final installer version labels and documentation identify it as Public Beta 2.
