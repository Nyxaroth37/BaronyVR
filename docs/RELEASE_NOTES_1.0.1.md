## BaronyVR 1.0.1 — Calibration and bottle interactions

An update to the native OpenXR PC VR mod by **ResidentMonster**.

### What's new

- Rebuilt VR calibration with native Barony artwork, category tabs and labeled sliders. Body settings control arm size/placement; weapons/items, crosshairs, bows and HUD have their own sections. Arm model and flail editors are no longer exposed.
- Headset-following belt placement in seated and standing play, and an opt-in room-scale tracking test under Body. Room-scale steps use native collision; stick movement follows headset heading in that mode.
- Bottles now equip by default in VR instead of immediately drinking. Held bottles are half their previous size.
- Physical drinking in ordinary offline single-player: equip a nonempty potion in the normal weapon slot, raise the right controller near your face with Grip released, then **press Grip**. One drink per press; release and press again to drink again. No tilt or dwell is needed. Keep the trigger released while drinking; normal trigger throwing remains available.
- Updated all five bundled defaults to ResidentMonster's latest saved calibration/settings. Fresh defaults use **Standing** mode and a right bow draw hand. Existing player calibration, settings and saves are preserved; only missing files are seeded.

### Install or update

Existing 1.0.0 users can open **BaronyVR.exe** and choose **Update and play**. Fresh installs: download **BaronyVR_1.0.1_Windows.zip**, extract all files, run **Setup.cmd**, confirm the retail Barony folder and install the launcher. The desktop shortcut remains **BaronyVR**.

The stable-release updater is unchanged. Downloads are verified, the previous runtime is retained for rollback, and cleanup remains limited to owned managed files. A source push alone does not update players.

### Compatibility and known limits

Windows x64, Steam Barony build **22630456**, PC-connected headset, active 64-bit OpenXR runtime and Visual C++ x64 runtime are required. Quest 3 with SteamVR/OpenXR is the verified development setup. Other headsets/controllers, other stores and standalone Quest play are unverified or unsupported as documented in the README. The package is unsigned.

- Physical drinking and experimental room-scale/world grabbing are limited to supported offline single-player contexts. Physical bow multiplayer limitations remain.
- Loose-object grabbing remains experimental and has not yet been confirmed working in the headset. Motion-based throwing and transferring grabbed objects into the backpack are not implemented. Native equipped-item throwing remains available.
- A thin overhead strip in room-scale and some peripheral pop-in remain unresolved. Visibility diagnostic switches remain under Controls & Help; they reset on restart. The culling changes do not alter headset optics or stretch the OpenXR projection.
- Room-scale tracking is an opt-in test and starts off each game launch; Standing play mode alone does not enable physical steps. Existing collision protections remain.
- First-run story presentation/skipping and broader multiplayer/controller combinations still need testing.

### Validation

The GA33 native Windows x64 Release build and right-grip drinking gesture were tested by the developer in the headset. Held bottle size was accepted separately. Production-block C++ tests cover input ownership, native movement integration, calibration logic, bottle scale and native-call lifetime/rearming. Runtime/source hashes and all five default files are recorded in release provenance. The release tooling additionally checks package integrity, fresh default seeding, preservation of edits and saves, update activation and rollback before publication, then the actual anonymous public download/apply path after publication.
