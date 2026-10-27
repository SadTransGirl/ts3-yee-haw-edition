"""Show what the system XInput sees, live, for ~15 seconds.

    python harness\check_pads.py

Run this after starting DSX (or whatever creates the virtual Xbox pad) and
BEFORE launching the game. Press buttons: if they show up here, the harness
will record them. If every pad says "not connected", the game won't see a
controller through the proxy either.
"""
import ctypes
import sys
import time

xi = ctypes.WinDLL("xinput1_4")  # loads the real System32 copy


class GAMEPAD(ctypes.Structure):
    _fields_ = [("buttons", ctypes.c_ushort), ("lt", ctypes.c_ubyte), ("rt", ctypes.c_ubyte),
                ("lx", ctypes.c_short), ("ly", ctypes.c_short), ("rx", ctypes.c_short), ("ry", ctypes.c_short)]


class STATE(ctypes.Structure):
    _fields_ = [("packet", ctypes.c_uint), ("gamepad", GAMEPAD)]


NAMES = {0x1: "Up", 0x2: "Down", 0x4: "Left", 0x8: "Right", 0x10: "Start", 0x20: "Back", 0x40: "LS",
         0x80: "RS", 0x100: "LB", 0x200: "RB", 0x1000: "A", 0x2000: "B", 0x4000: "X", 0x8000: "Y"}

end = time.time() + 15
print("polling XInput pads 0-3 for 15 s; press buttons on the controller...\n")
while time.time() < end:
    line = []
    for pad in range(4):
        st = STATE()
        if xi.XInputGetState(pad, ctypes.byref(st)) != 0:
            line.append(f"pad{pad}: not connected")
            continue
        g = st.gamepad
        btn = "+".join(n for b, n in NAMES.items() if g.buttons & b) or "-"
        line.append(f"pad{pad}: {btn} L({g.lx:6d},{g.ly:6d}) R({g.rx:6d},{g.ry:6d}) LT{g.lt:3d} RT{g.rt:3d}")
    sys.stdout.write("\r" + " | ".join(line)[:180].ljust(180))
    sys.stdout.flush()
    time.sleep(0.05)
print()
