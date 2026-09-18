#!/usr/bin/env python3
"""Capture a screenshot from the Dial Throttle over USB serial.

Usage:
  tools/screenshot.py [--port /dev/ttyACM0] [--setup "notch 5" --setup "..."] out.png
  tools/screenshot.py --decode dump.hex out.png

Needs pyserial to capture and Pillow to write PNGs. Without Pillow the raw hex dump is saved next
to the requested output (out.hex) and can be decoded later with --decode.
"""
import argparse
import sys
import time


def capture(port, setups, settle):
    import serial

    ser = serial.Serial(port, 115200, timeout=0.5)
    time.sleep(0.3)
    ser.reset_input_buffer()
    for cmd in setups:
        ser.write((cmd + "\n").encode())
        time.sleep(0.15)
    time.sleep(settle)
    ser.reset_input_buffer()
    ser.write(b"shot\n")
    header = None
    rows = []
    deadline = time.time() + 30
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if not line:
            continue
        if line.startswith("SHOT BEGIN"):
            _, _, w, h, bits = line.split()
            header = (int(w), int(h), int(bits))
            rows = []
            continue
        if line == "SHOT END":
            break
        if header is not None and all(c in "0123456789ABCDEF" for c in line):
            rows.append(line)
    ser.close()
    if header is None or len(rows) != header[1]:
        sys.exit(f"incomplete capture: header={header} rows={len(rows)}")
    return header, rows


def decode(header, rows, out_png):
    w, h, bits = header
    from PIL import Image

    img = Image.new("RGB", (w, h))
    px = img.load()
    for y, row in enumerate(rows):
        data = bytes.fromhex(row)
        for x in range(w):
            if bits == 16:
                v = (data[2 * x] << 8) | data[2 * x + 1]  # sprite stores big-endian RGB565
                r = (v >> 11) & 0x1F
                g = (v >> 5) & 0x3F
                b = v & 0x1F
                px[x, y] = ((r * 255) // 31, (g * 255) // 63, (b * 255) // 31)
            else:
                v = data[x]  # RGB332
                px[x, y] = (((v >> 5) & 7) * 255 // 7, ((v >> 2) & 7) * 255 // 7, (v & 3) * 255 // 3)
    img.save(out_png)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--setup", action="append", default=[], help="serial command to run before the shot")
    ap.add_argument("--settle", type=float, default=0.5, help="seconds to wait after setup commands")
    ap.add_argument("--decode", help="decode an existing .hex dump instead of capturing")
    args = ap.parse_args()

    if args.decode:
        with open(args.decode) as f:
            first = f.readline().split()
            header = (int(first[0]), int(first[1]), int(first[2]))
            rows = [l.strip() for l in f if l.strip()]
        decode(header, rows, args.out)
        return

    header, rows = capture(args.port, args.setup, args.settle)
    hex_path = args.out.rsplit(".", 1)[0] + ".hex"
    with open(hex_path, "w") as f:
        f.write(f"{header[0]} {header[1]} {header[2]}\n")
        f.write("\n".join(rows) + "\n")
    try:
        decode(header, rows, args.out)
        print(f"wrote {args.out}")
    except ImportError:
        print(f"Pillow not installed; saved {hex_path}. Decode with: tools/screenshot.py --decode {hex_path} {args.out}")


if __name__ == "__main__":
    main()
