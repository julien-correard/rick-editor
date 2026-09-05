#!/usr/bin/env python3
"""X11 helpers to drive the FS-UAE window on DISPLAY=:0 (python-xlib, no sudo).

Usage:
  xshot.py find                 list FS-UAE windows
  xshot.py shot <out.bmp>       screenshot the FS-UAE window (or root) as BMP
  xshot.py keys <window-id> F12 d
                                send key events (pressed+released) to window

Keys are X keysyms from keysymdef.h (case matters: use exact X keysym names).
"""
import sys
import struct
from Xlib import display, X, XK
from Xlib.ext import xtest


def windows(d):
    root = d.screen().root
    out = []

    def walk(w, depth=0):
        try:
            attrs = w.get_attributes()
            name = w.get_wm_name() or ""
        except Exception:
            return
        if attrs.map_state == X.IsViewable:
            try:
                geo = w.get_geometry()
                out.append((w, depth, name, geo))
            except Exception:
                pass
        for c in w.query_tree().children:
            walk(c, depth + 1)

    walk(root)
    return out


def write_png(path, data, w, h, stride=None):
    import zlib
    if stride is None:
        stride = (w * 3 + 3) // 4 * 4
    rows = []
    for y in range(h):
        row = bytearray([0])
        row += data[y * stride:(y + 1) * stride][:w * 3]
        rows.append(bytes(row))
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)
    idat = zlib.compress(b"".join(rows))

    def chunk(tag, payload):
        c = tag + payload
        return struct.pack(">I", len(payload)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", ihdr))
        f.write(chunk(b"IDAT", idat))
        f.write(chunk(b"IEND", b""))


def shot_rgb(w, geo):
    img = w.get_image(0, 0, geo.width, geo.height, X.ZPixmap, 0xFFFFFFFF)
    src = img.data
    if img.depth == 24:
        stride = (geo.width * 24 + 31) // 32 * 4
        return bytes(src), stride, geo.width, geo.height
    if img.depth == 32:
        stride = geo.width * 4
        return bytes(src), stride, geo.width, geo.height
    raise SystemExit(f"unsupported depth {img.depth}")


def to_ascii(w=None, cols=96, rows=48):
    d = display.Display(os.environ.get("DISPLAY", ":0"))
    if w is None:
        w = best_window(d)
        if w is None:
            return "no fs-uae window found"
    geo = w.get_geometry()
    raw, stride, pw, ph = shot_rgb(w, geo)
    ch = " .:-=+*#%@"
    out = []
    for ry in range(rows):
        line = []
        y0 = int(ph * ry / rows)
        for rx in range(cols):
            x0 = int(pw * rx / cols)
            x1 = max(x0 + 1, int(pw * (rx + 1) / cols))
            y1 = max(y0 + 1, int(ph * (ry + 1) / rows))
            s = 0
            n = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    o = yy * stride + xx * 3
                    r, g, b = raw[o], raw[o + 1], raw[o + 2]
                    s += 0.299 * r + 0.587 * g + 0.114 * b
                    n += 1
            v = s / n
            line.append(ch[min(9, int(v / 25.6))])
        out.append("".join(line))
    return "\n".join(out)


def best_window(d, name_hint="amiga"):
    best = None
    for w, depth, name, geo in windows(d):
        if not (name and name_hint.lower() in name.lower()):
            continue
        if geo.width < 300 or geo.height < 200:
            continue
        if best is None or geo.width * geo.height > best[0]:
            best = (geo.width * geo.height, w)
    return best[1] if best else None


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else "find"
    d = display.Display(os.environ.get("DISPLAY", ":0"))

    if cmd == "find":
        for w, depth, name, geo in windows(d):
            print(f"id={w.id:#x} depth={depth} name={name!r} {geo.width}x{geo.height} @{geo.x},{geo.y}")
        return

    if cmd == "ascii":
        print(to_ascii())
        return

    if cmd == "shot":
        out = sys.argv[2] if len(sys.argv) > 2 else "shot.png"
        if out.endswith(".png"):
            fmt = "png"
        else:
            fmt = "bmp"
        target = None
        if len(sys.argv) > 3:
            target = int(sys.argv[3], 0)
        else:
            # pick the deepest/largest window whose name mentions Amiga/FS-UAE
            best = None
            for w, depth, name, geo in windows(d):
                if not (name and (("amiga" in name.lower()) or ("uae" in name.lower()))):
                    continue
                if geo.width < 300 or geo.height < 200:
                    continue
                if best is None or geo.width * geo.height > best[0]:
                    best = (geo.width * geo.height, w.id)
            if best:
                target = best[1]
        if target is None:
            print("shot: no target window found", file=sys.stderr)
            sys.exit(1)
        w = d.create_resource_object("window", target)
        geo = w.get_geometry()
        if fmt == "png":
            raw, stride, pw, ph = shot_rgb(w, geo)
            write_png(out, raw, pw, ph, stride)
            print(f"saved {out} {pw}x{ph}")
        else:
            img = w.get_image(0, 0, geo.width, geo.height,
                              X.ZPixmap, 0xFFFFFFFF)
            raw, pw, ph = img.data, geo.width, geo.height
            write_bmp(out, raw, pw, ph, img.depth)
            print(f"saved {out} {pw}x{ph}")
        return

    if cmd == "keys":
        wid = int(sys.argv[2], 0)
        keysyms = sys.argv[3:]
        w = d.create_resource_object("window", wid)
        w.set_input_focus(X.RevertToParent, X.CurrentTime)
        d.sync()
        for ks in keysyms:
            sym = XK.string_to_keysym(ks)
            if not sym:
                print(f"unknown keysym {ks}", file=sys.stderr)
                sys.exit(1)
            code = d.keysym_to_keycode(sym)
            if not code:
                print(f"no keycode for {ks}", file=sys.stderr)
                sys.exit(1)
            xtest.fake_input(d, X.KeyPress, code)
            d.sync()
            xtest.fake_input(d, X.KeyRelease, code)
            d.sync()
        print(f"sent keys: {' '.join(keysyms)}")
        return

    print(f"unknown cmd {cmd}")
    sys.exit(1)


def write_bmp(path, data, w, h, depth=24):
    bpp = 3
    rowsize = ((w * bpp + 3) // 4) * 4
    # X lib returns data top-down (left-to-right, no scanline pad but X GetImage
    # returns 32bpp aligned on many servers; handle both common cases)
    imgdata = bytearray(rowsize * h)
    stride = (w * depth + 31) // 32 * 4 if depth == 24 else (w * depth + 31) // 32 * 4
    for y in range(h):
        for x in range(w):
            off = y * stride + x * (depth // 8)
            b = data[off] if depth >= 24 else data[off // (8 // depth)] >> (8 - depth - (x % (8 // depth)) * depth)
            if depth >= 24:
                r, g, bl = data[off + 2], data[off + 1], data[off]
            else:
                r = g = bl = b
            o = y * rowsize + x * 3
            imgdata[o + 0], imgdata[o + 1], imgdata[o + 2] = bl, g, r
    if not isinstance(imgdata, bytes):
        imgdata = bytes(imgdata)
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    hdr[2:6] = struct.pack("<I", 54 + len(imgdata))
    hdr[10:14] = struct.pack("<I", 54)
    hdr[14:18] = struct.pack("<I", 40)
    hdr[18:22] = struct.pack("<I", w)
    hdr[22:26] = struct.pack("<i", h)
    hdr[26:28] = struct.pack("<H", 1)
    hdr[28:30] = struct.pack("<H", 24)
    hdr[34:38] = struct.pack("<I", len(imgdata))
    with open(path, "wb") as f:
        f.write(hdr)
        f.write(imgdata)
    print(f"wrote {path} ({w}x{h})")


import os
main()