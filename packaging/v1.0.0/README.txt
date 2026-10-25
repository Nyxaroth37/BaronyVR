BARONYVR 1.0.0 - FIRST PUBLIC RELEASE
Unofficial native OpenXR mod. Retail Steam Barony is required.

INSTALL
Extract all files, then run Setup.cmd. Confirm your Steam Barony folder and
select Install launcher. Use Open launcher or the BaronyVR desktop/Start menu
shortcut afterward. The installed launcher is BaronyVR.exe.
Earlier prerelease users install this version manually once to get updates.

REQUIREMENTS
Windows x64; Steam Barony build 22630456; an active 64-bit OpenXR runtime;
Microsoft Visual C++ x64 runtime. Quest 3 with SteamVR is the verified setup.
Other headsets/controller profiles are unverified. Epic/GOG builds and Quest
standalone play are not supported. This package is unsigned.

SETTINGS
ResidentMonster's five saved VR configuration files are the defaults.
Existing settings are preserved: a default is copied only when missing.
Defaults use seated mode and a right draw hand; adjust them in calibration.
Calibration, ordinary settings, and saves remain in retail Barony.
Retail EXE/DLLs and artwork are not replaced. Steam and FMOD load from retail.
The retail folder must remain installed and writable by the player.

UPDATES
The launcher checks published stable releases without a GitHub login.
Drafts, prereleases, and source commits do not update players.
Update and play verifies downloads and switches versions after validation.
Play without updating remains available while checks run or when offline.
Use previous version switches to the retained rollback runtime.
The active and one rollback runtime are retained. Completed temporary work
is removed; abandoned updater work is bounded and ages out after 24 hours.
Unexpected files and linked folders block deletion. Retail files, calibration,
saves, and development backups are never updater cleanup targets.
Verified legacy installer/launcher folders are removed after successful exit
with a rollback runtime retained. Recognized beta shortcuts migrate to BaronyVR.

FILES AND REMOVAL
Launcher: %LOCALAPPDATA%\BaronyVR\Managed\Launcher
Versions: %LOCALAPPDATA%\BaronyVR\Managed\Versions
To remove: close the game/launcher, delete the BaronyVR shortcuts, and remove
the Managed folder. Keep retail calibration files to retain settings.
Vanilla Barony remains installed. The shortcut reuses Barony's existing icon.

LIMITATIONS AND REPORTS
Accepted gameplay baseline: GA21. Physical shortbow is for singleplayer/host;
clients retain native fallback. Full VR bow aim networking is not implemented.
Offhand presentation, first-run story display/skipping, broader multiplayer,
and other headset/controller combinations still need testing.
Logs/crashlogs are in retail Barony. Review personal information before sharing.
Report issues: https://github.com/Nyxaroth37/BaronyVR/issues
Source and documentation: https://github.com/Nyxaroth37/BaronyVR
See Notices for component licenses. No proprietary retail DLLs or assets bundled.
