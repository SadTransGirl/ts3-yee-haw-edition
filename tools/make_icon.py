"""Make the launcher's Windows icon from the square icon art.

    python -I tools/make_icon.py docs/icon.png launcher/Assets/YeeHaw.ico

The .ico holds every size Windows asks for (Explorer, the taskbar, Alt+Tab, the title bar), each
resampled from the 512 px render (launcher --icon) rather than letting Windows shrink one image.
"""
import sys

from PIL import Image

SIZES = [16, 20, 24, 32, 40, 48, 64, 96, 128, 256]

src = Image.open(sys.argv[1]).convert("RGBA")
if src.width != src.height:
    raise SystemExit("icon art must be square (it's %dx%d)" % src.size)
src.save(sys.argv[2], format="ICO", sizes=[(s, s) for s in SIZES])
print("wrote %s: %s px" % (sys.argv[2], ", ".join(str(s) for s in SIZES)))
