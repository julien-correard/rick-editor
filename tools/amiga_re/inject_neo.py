#!/usr/bin/env python3
"""Inject twi2.neo + a 68k display stub into emulated chip RAM via the UAE
debugger FIFO, then start execution in the stub. Displays the .NEO bitmap
on the Amiga 320x200 4-bitplane screen for format validation.

Usage:
  inject_neo.py <debugger_fifo> [--img extracted/twi2.neo]

Addresses (chip RAM):
  0x00004000  stub code
  0x00008000  spare (Copper, unused - CPU writes regs directly)
  0x00010000  twi2.neo file image (header 0x80 bytes, then 4 planes x 8000)
"""
import sys
import time
import struct

CODE = 0x70000
NEO = 0x74000
DFF = 0x00DFF000

# ---- assemble minimal 68k stub (CPU writes all display regs directly) ----
# Encode with explicit bytes; verify via capstone after.

def w16(b, v):
    if len(b) & 1:
        b.append(0)
    b += list(struct.pack(">H", v & 0xFFFF))

def w32(b, v):
    if len(b) & 1:
        b.append(0)
    b += list(struct.pack(">I", v & 0xFFFFFFFF))

def imm_to_abs_w(code, imm, addr):
    w16(code, 0x33FC)        # MOVE.W #imm,(abs.l)
    w16(code, imm)
    w32(code, addr)

def imm_to_abs_l(code, imm, addr):
    w16(code, 0x23FC)        # MOVE.L #imm,(abs.l)
    w32(code, imm)
    w32(code, addr)

# hand-assembled list of (kind,...) operations
ops = []
# disable interrupts / DMA helpers
ops.append(("iw", 0x7FFF, DFF + 0x9A))   # INTENA: disable all
ops.append(("iw", 0x7FFF, DFF + 0x96))   # DMACON: disable blitter/sprites

ops.append(("iw", 0x2C81, DFF + 0x8E))   # DIWSTRT
ops.append(("iw", 0x2CC1, DFF + 0x90))   # DIWSTOP
ops.append(("iw", 0x0038, DFF + 0x92))   # DDFSTRT
ops.append(("iw", 0x00D0, DFF + 0x94))   # DDFSTOP
ops.append(("iw", 0x0000, DFF + 0x108))  # BPL1MOD
ops.append(("iw", 0x0000, DFF + 0x10A))  # BPL2MOD

# plane pointers (8 ops, move.l #addr -> BPLxPT)
plane_base = NEO + 0x80
for p in range(4):
    ops.append(("il", plane_base + p * 8000, DFF + 0xE0 + p * 4))  # BPL0..BPL3PT

ops.append(("iw", 0x3200, DFF + 0x100))  # BPLCON0 = 4 planes, lowres

def build_stub():
    code = []
    for op in ops:
        if op[0] == "iw":
            imm_to_abs_w(code, op[1], op[2])
        else:
            imm_to_abs_l(code, op[1], op[2])
    # palette writes appended later (after reading image); jump is below
    return code

def add_palette(code, pal16):
    for i, c in enumerate(pal16):
        imm_to_abs_w(code, c, DFF + 0x180 + i * 2)

def add_loop(code):
    w16(code, 0x60FE)   # BRA.S *  (infinite loop)

def main():
    fifo = sys.argv[1]
    img = "extracted/twi2.neo"
    if len(sys.argv) > 2 and sys.argv[2].startswith("--img="):
        img = sys.argv[2].split("=", 1)[1]
    d = open(img, "rb").read()

    # parse geometry from header (width/height at ~0x38/0x3c) -> use 320x200
    # palette: 16 color words at header offset 0x08..? We observed words at
    # bytes 8..34 (colors 0..13) then zeros. Use low 12 bits of each word.
    pal = []
    for i in range(16):
        w = struct.unpack(">H", d[8 + i * 2:8 + i * 2 + 2])[0] if (8 + i * 2 + 2) <= 0x80 else 0
        pal.append(w & 0xFFF)          # Amiga COLORnn = 0RGB (12-bit)
    print("palette(16):", ["%03x" % c for c in pal])

    stub = build_stub()
    add_palette(stub, pal)
    add_loop(stub)
    # pad to even
    if len(stub) & 1:
        stub.append(0)

    # sanity: disassemble to verify
    try:
        from capstone import Cs, CS_ARCH_M68K, CS_MODE_M68K_000
        md = Cs(CS_ARCH_M68K, CS_MODE_M68K_000)
        ins = list(md.disasm(bytes(stub), CODE))
        print("stub %d bytes, %d instructions" % (len(stub), len(ins)))
        for it in ins[:24]:
            print("   %08x: %-14s %s" % (it.address, it.mnemonic, it.op_str))
    except Exception as e:
        print("disasm skipped:", e)

    # ---- build W command streams ----
    # Write stub code
    w_cmds = []
    w_cmds.append(_build_w(CODE, stub))
    # Write NEO image
    data = list(d)
    if len(data) & 1:
        data.append(0)
    w_cmds.append(_build_w(NEO, data))

    fifo_fd = open(fifo, "wb")
    print("writing stub code + image via debugger W (single write) ...")
    buf = []
    for chunk in chunk_cmds(w_cmds):
        buf.append((chunk + "\n"))
    # start CPU at stub (this also leaves the debugger to run)
    buf.append(("g %.6x\n" % CODE))
    data = "".join(buf).encode()
    fifo_fd.write(data)
    fifo_fd.flush()
    print("wrote %d bytes, started CPU @ %#x" % (len(data), CODE))

def _build_w(addr, words):
    # W command: "W <address> <values...>" each value is 16-bit by default
    # Split into lines of up to 16 values each.
    lines = []
    vals = ["%04x" % v for v in words]
    for i in range(0, len(vals), 14):
        a = addr + i * 2
        lines.append("W %.6x %s" % (a, " ".join(vals[i:i + 14])))
    return "\n".join(lines)

def chunk_cmds(cmd_list):
    for c in cmd_list:
        for line in c.split("\n"):
            if line.strip():
                yield line

main()
