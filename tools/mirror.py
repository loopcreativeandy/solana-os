#!/usr/bin/env python3
"""Show the badge's screen live on the computer (console command "mirror", core/os_mirror.c).

    python3 tools/mirror.py --port COM5                    # window, 115200 baud
    python3 tools/mirror.py --port COM5 --baud 921600      # faster: badge and port switch speed together
    python3 tools/mirror.py --port COM5 --png frames/      # also save every complete frame as a PNG
    python3 tools/mirror.py --file log.txt --png frames/   # decode a saved console log, no badge

The window is a plain Tk window (OBS: Window Capture). Closing it sends "mirror off", which also puts the
badge back to its normal speed; if this script dies at a high speed, reboot the badge or reconnect with
--baud <that speed> and close the window. Needs pyserial; --png needs Pillow.
"""
import argparse, base64, os, sys, time

W, H = 320, 240


class Screen:
    def __init__(self):
        self.rgb = bytearray(W * H * 3)
        self.area = None
        self.data = bytearray()
        self.frames = 0

    def line(self, line):
        """Feed one console line; True when a frame is complete."""
        if not line.startswith("MIR "):
            return False
        p = line.split()
        if p[1] == "ON":
            global W, H
            w, h = int(p[2]), int(p[3])
            if (w, h) != (W, H):
                W, H = w, h
                self.rgb = bytearray(W * H * 3)
        elif p[1] == "A":
            self.area = tuple(int(v) for v in p[2:6])
            self.data = bytearray()
        elif p[1] == "D" and self.area and len(p) > 2:
            self.data += base64.b64decode(p[2])
        elif p[1] == "E" and self.area:
            self.paint()
            self.area = None
        elif p[1] == "F":
            self.frames += 1
            return True
        return False

    def paint(self):
        x0, y0, w, h = self.area
        d, i, n = self.data, 0, 0
        while i + 2 < len(d) and n < w * h:
            count, c = d[i] + 1, d[i + 1] | d[i + 2] << 8
            i += 3
            r, g, b = c >> 11, (c >> 5) & 63, c & 31
            px = bytes(((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)))
            for k in range(n, min(n + count, w * h)):
                x, y = x0 + k % w, y0 + k // w
                if x < W and y < H:
                    o = (y * W + x) * 3
                    self.rgb[o:o + 3] = px
            n += count

    def ppm(self):
        return b"P6 %d %d 255\n" % (W, H) + bytes(self.rgb)

    def save(self, folder):
        from PIL import Image
        os.makedirs(folder, exist_ok=True)
        path = os.path.join(folder, f"frame_{self.frames:05d}.png")
        Image.frombytes("RGB", (W, H), bytes(self.rgb)).save(path)
        return path


def from_file(a):
    scr = Screen()
    for line in open(a.file, encoding="utf-8", errors="replace"):
        if scr.line(line.strip()) and a.png:
            print(scr.save(a.png))
    if not scr.frames:
        print("no complete frame in the log", file=sys.stderr)
        sys.exit(1)


def live(a):
    import serial, tkinter as tk
    s = serial.Serial()
    s.port, s.baudrate, s.timeout = a.port, 115200, 0
    # Don't touch DTR/RTS: setting them to False drops DTR first, so for a moment only RTS is on,
    # which is the esptool reset signal (the badge rebooted on every connect).
    s.open()
    # Some systems still reset the badge on open: if it's booting, wait for its console first,
    # otherwise "mirror on" is lost during the boot and the window stays black.
    head, t0 = b"", time.time()
    while time.time() - t0 < 1.5:
        head += s.read(4096); time.sleep(0.02)
    if b"ESP-ROM" in head or b"rst:" in head:
        print("badge is rebooting, waiting for the console...")
        t0 = time.time()
        while b"console ready" not in head and time.time() - t0 < 20:
            head += s.read(4096); time.sleep(0.02)
        time.sleep(1.5)
    s.reset_input_buffer()
    s.write(b"\r\nmirror on" + (b" %d" % a.baud if a.baud != 115200 else b"") + b"\r\n")

    scr = Screen()
    root = tk.Tk()
    root.title("Solana OS badge (waiting for the first frame...)")
    img = tk.PhotoImage(width=W, height=H)
    shown = img.zoom(a.zoom)
    label = tk.Label(root, image=shown, bd=0)
    label.pack()
    buf = b""

    def poll():
        nonlocal buf, shown
        buf += s.read(65536)
        *lines, buf = buf.split(b"\n")
        frame = False
        for raw in lines:
            line = raw.decode("utf-8", "replace").strip()
            if line.startswith("MIR BAUD "):
                time.sleep(0.05)
                s.baudrate = int(line.split()[2])
                buf = b""
            elif line.startswith("MIR "):
                frame |= scr.line(line)
            elif line and not a.quiet:
                print(line)
        if frame:
            img.configure(data=scr.ppm(), format="PPM")
            shown = img.zoom(a.zoom)
            label.configure(image=shown)
            if scr.frames == 1:
                root.title("Solana OS badge")
            if a.png:
                scr.save(a.png)
        root.after(15, poll)

    def close():
        s.write(b"\r\nmirror off\r\n")
        s.flush()
        time.sleep(0.3)
        s.close()
        root.destroy()

    root.protocol("WM_DELETE_WINDOW", close)
    root.after(15, poll)
    root.mainloop()


p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument("--port", help="serial port, e.g. COM5 or /dev/ttyUSB0")
p.add_argument("--baud", type=int, default=115200, help="mirror speed (CH340C: up to 2000000)")
p.add_argument("--zoom", type=int, default=3, help="window scale")
p.add_argument("--png", help="folder: save every complete frame")
p.add_argument("--file", help="decode a saved console log instead of a live badge")
p.add_argument("--quiet", action="store_true", help="don't echo other console output")
a = p.parse_args()
if a.file:
    from_file(a)
elif a.port:
    live(a)
else:
    p.error("--port or --file")
