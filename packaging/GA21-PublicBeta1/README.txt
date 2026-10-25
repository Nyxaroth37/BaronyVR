BARONY VR - GA21 PUBLIC BETA 1
Unofficial native OpenXR mod. Retail Barony is required.

INSTALL
Extract the whole ZIP, then double-click Setup.cmd. Confirm the detected
Steam Barony folder and select Install beta. Connect your headset to your
PC, start Steam and your active OpenXR runtime, then select Play in VR.
Later, use the Barony VR - GA21 Beta desktop or Start menu shortcut.
The shortcut opens the installed BaronyVR.exe launcher. It can also be
started directly from the installed folder. Keep it beside Launch.ps1.

Upgrading a private Beta 1 or Beta 1 R1: setup redirects only recognized Beta 1 shortcuts and
backs them up in ShortcutBackups. The previous installation is retained.
The extracted setup folder can be removed after a successful installation.

REQUIREMENTS AND SUPPORT
Windows x64; Steam retail Barony build 22630456; a PC-connected VR headset;
an active 64-bit OpenXR runtime; Microsoft Visual C++ x64 runtime.
No Git, Visual Studio, source download, or game compilation is required.
Quest 3 with SteamVR is the verified development setup. Quest 2 and other
headsets/controller profiles have not been verified. This is a Windows
PC VR mod, not a standalone Quest application. Epic/GOG retail builds
are not supported by this beta.

YOUR SETTINGS
Five supplied VR configuration files are Mike's actual saved defaults.
Setup creates a file only if that file does not already exist in retail
Barony. Existing player calibration is never replaced by setup or launch.
Defaults use seated mode and a right draw hand (bow held in the left hand).
Players can change these through the existing VR calibration controls.
The defaults include personal arm proportions and HUD placement: adjust
them to fit your own body, headset, and preferred position.

This version shares the retail game's ordinary settings and saves.
It does not create an isolated save profile. A new retail profile can show
the opening story; full VR intro presentation/skipping remains to be tested.

CONTROLS AND LIMITS
Right thumb-rest touch + left stick sends cardinal D-pad input. Lift the
thumb to return the stick to movement. Requires a compatible touch binding.
Right grip retains native bumper interactions when not owned by another
VR interaction. Quick Turn is on right-stick down, not right grip.
For the physical shortbow, hold the draw-hand grip near the string, pull
back, hold, and release grip to shoot. Cancel the draw before opening menus.
The physical bow path is currently for shortbow on singleplayer/host.
Multiplayer clients retain the native fallback; full VR bow network aiming
is not claimed. Offhand mesh visibility with the bow remains limited.
Existing VR combat, blocking, wrist HUD, minimap, belt, skill notifications,
and calibration are the accepted GA21 implementation.

FILES AND UPDATES
Mod binaries install under %LOCALAPPDATA%\BaronyVR\GA21-PublicBeta1.
The retail EXE, DLLs, and artwork are not replaced. Steam and FMOD DLLs are
loaded from the player's own retail installation; they are not distributed.
Calibration files, game settings, logs, and saves use the retail folder.
Use the same Windows account that normally plays Barony and has write
access to that folder. Setup never automatically requests administrator rights.
Retail version mismatches stop launch until a compatible beta is available.
This beta is unsigned. No automatic downloads or updates are performed.

REMOVE
Close Barony VR. Delete the Barony VR - GA21 Beta shortcuts from your desktop
and Start menu, then delete %LOCALAPPDATA%\BaronyVR\GA21-PublicBeta1.
Keep the retail calibration files if you want to retain your settings.
Do not delete retail assets or saves. Vanilla Barony remains installed.

REPORT A PROBLEM
Include the beta version, retail build, headset, OpenXR runtime, and steps
to reproduce. log.txt and crashlogs are in your retail Barony folder.
Logs may contain local paths and account/device information; review them
before sharing publicly. Never upload your retail artwork or game files.

SOURCE AND LICENSES
Accepted source commit: 803239368ebbf726f8669a506892089391d0a540
Public repository: https://github.com/Nyxaroth37/BaronyVR
Baseline tag: vr-ga21-baseline
The public repository contains the GA21 source snapshot and installer/launcher
source. The original development history remains private. See Notices for bundled
component copyright and license texts. Retail Barony is required separately.
