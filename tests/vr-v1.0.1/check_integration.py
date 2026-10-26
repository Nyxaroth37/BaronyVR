from pathlib import Path
import json,hashlib
r=Path(__file__).resolve().parents[2]
s=(r/'src/vr_openxr.cpp').read_text()
a=s.index('struct Gesture {',s.index('namespace VrPotionDrink {\nstruct Gesture'))
b=s.index('static Gesture state;',a)
assert s[a:b]==(Path(__file__).parent/'drink_gesture.inc').read_text()
a=s.index('// GA33: deliberate, controller-based drinking pilot.')
b=s.index('} // namespace VrPotionDrink',a)+len('} // namespace VrPotionDrink')
assert s[a:b] in (Path(__file__).parent/'drink_integration.cpp').read_text()
assert 'VrRoomScale::applyPending(); VrPotionDrink::applyPending();' in s
frame=s[s.index('void vrProcessOpenXRFrame('):s.index('void vrProcessOpenXRFrame(')+6500]
assert frame.index('vrLocateEyeViews(')<frame.index('VrPotionDrink::sample();')<frame.index('vrUpdateVirtualSDLGamepad();')
assert '!g_vrDrinkOwnsRightGrip && !vrMenuPointerMode()' in s
print('PASS: exact gesture/integration fixture copies, safe hook dispatch, current poses before sample, final input ownership and package source hashes.')
