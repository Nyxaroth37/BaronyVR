BARONYVR 1.0.1 - CALIBRATION AND BOTTLE INTERACTIONS
Unofficial native OpenXR mod by ResidentMonster. Retail Steam Barony required.

INSTALL / UPDATE
Existing 1.0.0 users: open BaronyVR.exe and select Update and play.
Fresh installs: extract ALL files, run Setup.cmd, confirm the retail Barony
folder, and select Install launcher. Use the BaronyVR desktop/Start menu
shortcut. Git, Visual Studio and compilation are not needed for players.

REQUIREMENTS
Windows x64; Steam Barony build 22630456; a PC-connected headset; active
64-bit OpenXR runtime; Microsoft Visual C++ x64 runtime. Quest 3 with SteamVR
is verified. Other headset/controller profiles are unverified. Epic/GOG and
Quest standalone are not supported. This package is unsigned.

CALIBRATION AND CONTROLS
Press X+Y for the VR calibration menu. Use tabs and labeled sliders for body,
arm size/placement, weapons/items, crosshairs, bows and HUD. Protected arm-model
and flail editors are no longer exposed. Right thumb-rest touch plus left stick
provides D-pad input; lift your thumb to return to movement. Right-stick down
performs Quick Turn; grip retains other native interactions when not reserved.
The belt follows the headset. Body contains an opt-in room-scale tracking test;
physical steps start OFF each game launch even when play mode is Standing.

BOTTLES
Selecting a bottle in VR equips it instead of automatically drinking. Held
bottles are 50% smaller. In offline single-player, equip a nonempty potion in
the normal right weapon slot, raise the right controller near your face with
Grip RELEASED, then PRESS Grip to drink once. Release and press again for
another drink. No tilt or hold time is required. Keep trigger released while
drinking; native trigger attack/throw remains available. Empty bottles cannot
be drunk. Native potion effects and consumption rules apply.

DEFAULTS AND SAVES
All five bundled VR files are ResidentMonster's latest saved defaults, including
Standing mode and a right bow draw hand. Adjust them for your own proportions.
A default is copied only if its file is missing; updates never replace existing
player calibration. Ordinary settings and saves stay in retail Barony.
The retail EXE/DLLs and artwork are not replaced. Steam/FMOD load from retail;
the retail installation must remain present and writable for settings/saves.

UPDATES, ROLLBACK AND CLEANUP
Launcher checks published stable GitHub releases without a GitHub login.
Drafts, prereleases and source commits do not update players. Update and play
verifies downloads; Play without updating works offline. Use previous version
switches to the retained rollback runtime. Active plus one rollback version
are retained. Only verified owned managed folders are cleanup targets;
calibration, saves, retail files and unexpected user files are preserved.

KNOWN LIMITS
Drinking and room-scale/world-grab pilots are offline single-player only.
Loose-object grabbing is experimental and not headset-confirmed. Motion-based
throwing and backpack transfer are not implemented. A thin room-scale overhead
strip and peripheral pop-in remain unresolved. Temporary visibility diagnostics
are under Controls & Help and reset on restart. Bow multiplayer/aim-sync and
offhand limits, first-run story display/skipping, and broader controller support
remain. See GitHub release notes and README for details.

FILES / SUPPORT
Launcher: %LOCALAPPDATA%\BaronyVR\Managed\Launcher
Versions: %LOCALAPPDATA%\BaronyVR\Managed\Versions
Remove the mod by closing game/launcher, removing its shortcuts, and deleting
the Managed folder. Keep retail calibration files to retain settings.
Issues: https://github.com/Nyxaroth37/BaronyVR/issues
Source: https://github.com/Nyxaroth37/BaronyVR
See Notices for component licenses. No proprietary retail DLLs/assets bundled.
