# SPDX-License-Identifier: GPL-3.0-or-later
"""Protocol parity check: capture the bytes the upstream Python revision A
driver would send for a solid red 320x480 image and dump them to a file.

Requires a local checkout of turing-smart-screen-python (path in UPSTREAM).
"""
import os
import sys
import threading

upstream = os.environ.get("UPSTREAM", os.path.expanduser("~/src/turing-smart-screen-python"))
sys.path.insert(0, upstream)

from PIL import Image

from library.lcd.lcd_comm import Orientation
from library.lcd.lcd_comm_rev_a import LcdCommRevA

captured = bytearray()


def capture(buf):
    captured.extend(bytes(buf))


lcd = object.__new__(LcdCommRevA)
lcd.update_queue = None
lcd.update_queue_mutex = threading.Lock()
lcd.orientation = Orientation.PORTRAIT
lcd.display_width = 320
lcd.display_height = 480
lcd.image_cache = {}
lcd.font_cache = {}
lcd.text_bbox_cache = {}
lcd.lcd_serial = None
lcd.WriteData = capture
lcd.WriteLine = capture
lcd.serial_write = capture

image = Image.new("RGB", (320, 480), (255, 0, 0))
lcd.DisplayPILImage(image, 0, 0)

with open(sys.argv[1], "wb") as fh:
    fh.write(bytes(captured))
print("python: %d bytes" % len(captured))
