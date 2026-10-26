#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""PowerHub のシリアル出力を読み、JPEG_BEGIN〜JPEG_END の base64 を JPEG に復元する。
使い方: decode_jpeg.py /dev/ttyACM0 out.jpg"""
import base64, sys, serial

port, out = sys.argv[1], sys.argv[2]
s = serial.Serial(port, 115200, timeout=1)
data, rec = [], False
while True:
    line = s.readline().decode(errors="replace").strip()
    if line:
        print(line)
    if line.startswith("JPEG_BEGIN"):
        data, rec = [], True
    elif line == "JPEG_END" and rec:
        open(out, "wb").write(base64.b64decode("".join(data)))
        print("saved", out)
        break
    elif rec:
        data.append(line)
