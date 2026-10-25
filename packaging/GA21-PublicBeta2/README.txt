BARONY VR - GA21 PUBLIC BETA 2
Update-enabled launcher. Same accepted GA21 gameplay and saved defaults.

Extract all files and run Setup.cmd. Existing retail calibration and saves
are preserved. Setup installs a fixed BaronyVR.exe launcher under:
  %LOCALAPPDATA%\BaronyVR\Managed\Launcher
Runtime versions are under:
  %LOCALAPPDATA%\BaronyVR\Managed\Versions
Use the existing Barony VR desktop shortcut to open the new launcher.

The launcher checks published GitHub releases without a GitHub login.
Update and play downloads a compatible package, verifies GitHub's SHA256
digest and the package manifest, installs it separately, and switches the
active version only after verification. Source commits alone do not update
players. The beta channel includes compatible published prereleases.

Play without updating works while the check is running or when offline.
Use previous version switches to the retained rollback version. Calibration
and saves stay in retail Barony and are never reset by version switching.

CLEANUP
Keep the current and one previous managed runtime. Successful download,
staging, and check folders are removed. At later launcher starts, abandoned
temporary work is capped at three recent folders per category; older work
is removed after 24 hours. Only ownership-marked updater folders are eligible.
Unrecognized files, linked folders, or ownership mismatches stop cleanup for
that folder. Retail Barony, saved games, calibration, and development backups
are never cleanup targets. Retained-folder reasons use one cleanup-status.txt.
State backup files have a fixed name instead of accumulating numbered backups.

The exact original Public Beta 1 install can be copied into the managed
versions area for first-update rollback. Its original folder is removed only
after a successful game exit and verification of the retained managed copy.
Unrelated private test folders and development exports are not removed.

REQUIREMENTS AND LIMITS
Windows x64; Steam retail Barony build 22630456; an active 64-bit OpenXR
runtime; Microsoft Visual C++ x64 runtime. Quest 3 with SteamVR is the verified
development headset setup. Other headsets/store builds remain unverified or
unsupported. This is PC VR, not a standalone Quest app. The package is unsigned.
Existing GA21 bow, HUD, combat, networking, and intro limitations are unchanged.

Future releases must include the protocol-1 baronyvr-update.json descriptor
and a compatible Windows ZIP with payload update_protocol/release_sequence
metadata. Beta 1 has no descriptor and is ignored by automatic update lookup.
Future packages that require a newer launcher open the official release page
instead of attempting an incompatible automatic install.

No proprietary retail DLLs or game assets are bundled. See Notices for licenses.
Game source baseline: 803239368ebbf726f8669a506892089391d0a540
Public source repository: https://github.com/Nyxaroth37/BaronyVR

Default calibration profile: ResidentMonster. Player adjustments are preserved.

R1 cleanup recognizes installer-created shortcut backups only after checking
their exact filename pattern and Windows Shell Link header. Unknown files
block deletion. The verified prior launcher backup is removed after the first
successful game exit; only one such backup is retained during migration.

R2 keeps the launcher message loop running while hidden during gameplay,
so successful game exits can trigger completion and cleanup. A Windows
Forms regression test exercises the actual production message-loop helper.
