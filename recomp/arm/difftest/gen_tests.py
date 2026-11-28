#!/usr/bin/env python3
"""Generate differential test calls for a lifted ARM library.

Usage: gen_tests.py <lib.so> <out.txt> [seed]
Each output line is one call:  <addr> <name> | <args> | <buffers>
  addr  : guest function address in hex (Thumb bit kept)
  args  : words separated by spaces; `B<n>` = pointer to buffer n
  bufs  : hex-encoded initial contents of each buffer, space separated (`-` when none)
Both runners (Unicorn: run_unicorn.py; lifted build: SELFTEST_CALLS) read this file and print, per line,
`r0 r1 <buffer hex ...>` so the outputs can be compared (compare.py).
"""
import random
import struct
import sys

from elftools.elf.elffile import ELFFile


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", x))[0]


def d2q(x):
    return struct.unpack("<Q", struct.pack("<d", x))[0]


def fl(n, rnd, lo=-1000.0, hi=1000.0):
    return b"".join(struct.pack("<f", rnd.uniform(lo, hi)) for _ in range(n))


def main():
    so, out = sys.argv[1], sys.argv[2]
    rnd = random.Random(int(sys.argv[3]) if len(sys.argv) > 3 else 1)
    f = ELFFile(open(so, "rb"))
    sym = {}
    for s in f.get_section_by_name(".symtab").iter_symbols():
        if s["st_info"]["type"] == "STT_FUNC" and s["st_value"]:
            sym.setdefault(s.name, s["st_value"])
    lines = []

    def add(name, args, bufs=()):
        if name not in sym:
            return
        a = " ".join(x if isinstance(x, str) else f"0x{x & 0xFFFFFFFF:X}" for x in args)
        b = " ".join(x.hex() for x in bufs) or "-"
        lines.append(f"{sym[name]:X} {name} | {a} | {b}")

    special_f = [0.0, -0.0, 1.0, -1.0, 0.5, 3.14159265, 1e-30, 1e30, float("inf"), 123456.789, -7.25]
    special_d = [0.0, -0.0, 1.0, -1.0, 0.1, 2.5e-300, 1e300, 3.141592653589793, -123456.789, 1 / 3]
    # ---- soft-float helpers (results in r0 / r1:r0)
    for _ in range(60):
        a, b = rnd.choice(special_f + [rnd.uniform(-1e4, 1e4)] * 3), rnd.choice(special_f + [rnd.uniform(-1e4, 1e4)] * 3)
        for op in ("__aeabi_fadd", "__aeabi_fsub", "__aeabi_fmul", "__aeabi_fdiv", "__aeabi_fcmplt", "__aeabi_fcmpeq",
                   "__aeabi_fcmpgt", "__aeabi_fcmple", "__aeabi_fcmpge"):
            add(op, [f2u(a), f2u(b)])
        add("__aeabi_f2d", [f2u(a)])
        add("__aeabi_f2iz", [f2u(max(min(a, 2e9), -2e9))])
        add("__aeabi_i2f", [rnd.randint(-2**31, 2**31 - 1)])
        add("__aeabi_ui2f", [rnd.randint(0, 2**32 - 1)])
        x, y = rnd.choice(special_d + [rnd.uniform(-1e6, 1e6)] * 3), rnd.choice(special_d + [rnd.uniform(-1e6, 1e6)] * 3)
        qx, qy = d2q(x), d2q(y)
        for op in ("__aeabi_dadd", "__aeabi_dsub", "__aeabi_dmul", "__aeabi_ddiv", "__aeabi_dcmplt", "__aeabi_dcmpeq",
                   "__aeabi_dcmpgt", "__aeabi_dcmple", "__aeabi_dcmpge"):
            add(op, [qx & 0xFFFFFFFF, qx >> 32, qy & 0xFFFFFFFF, qy >> 32])
        add("__aeabi_d2f", [qx & 0xFFFFFFFF, qx >> 32])
        add("__aeabi_d2iz", [d2q(max(min(x, 2e9), -2e9)) & 0xFFFFFFFF, d2q(max(min(x, 2e9), -2e9)) >> 32])
        add("__aeabi_i2d", [rnd.randint(-2**31, 2**31 - 1)])
        add("__aeabi_ui2d", [rnd.randint(0, 2**32 - 1)])
    # ---- cocos2d math (buffers of floats)
    for _ in range(25):
        m1, m2 = fl(16, rnd, -10, 10), fl(16, rnd, -10, 10)
        add("_ZN7cocos2d9MathUtilC14multiplyMatrixEPKfS2_Pf", ["B0", "B1", "B2"], [m1, m2, bytes(64)])
        add("_ZN7cocos2d8MathUtil14multiplyMatrixEPKfS2_Pf", ["B0", "B1", "B2"], [m1, m2, bytes(64)])
        add("_ZN7cocos2d8MathUtil14multiplyMatrixEPKffPf", ["B0", f2u(rnd.uniform(-5, 5)), "B1"], [m1, bytes(64)])
        v = fl(4, rnd, -10, 10)
        add("_ZN7cocos2d9MathUtilC13transformVec4EPKfS2_Pf", ["B0", "B1", "B2"], [m1, v, bytes(16)])
        add("_ZN7cocos2d8MathUtil13transformVec4EPKfS2_Pf", ["B0", "B1", "B2"], [m1, v, bytes(16)])
        add("_ZN7cocos2d8MathUtil13transformVec4EPKfffffPf", ["B0", f2u(1.5), f2u(-2.0), f2u(0.25), "B1", "B2"],
            [m1, struct.pack("<f", 1.0), bytes(16)])
        a3, b3 = fl(3, rnd, -10, 10), fl(3, rnd, -10, 10)
        add("_ZN7cocos2d8MathUtil9crossVec3EPKfS2_Pf", ["B0", "B1", "B2"], [a3, b3, bytes(12)])
        ang = rnd.uniform(-7, 7)
        for axis in "XYZ":
            add(f"_ZN7cocos2d4Mat415createRotation{axis}EfPS0_", [f2u(ang), "B0"], [bytes(64)])
        add("_ZNK7cocos2d4Mat47rotateZEfPS0_", ["B0", f2u(ang), "B1"], [m1, bytes(64)])
        q1, q2 = fl(4, rnd, -1, 1), fl(4, rnd, -1, 1)
        add("_ZN7cocos2d10Quaternion8multiplyERKS0_S2_PS0_", ["B0", "B1", "B2"], [q1, q2, bytes(16)])
        add("_ZN7cocos2d4Mat414createRotationERKNS_10QuaternionEPS0_", ["B0", "B1"], [q1, bytes(64)])
        add("_ZN7cocos2d10Quaternion19createFromAxisAngleERKNS_4Vec3EfPS0_", ["B0", f2u(ang), "B1"], [a3, bytes(16)])
        v2, p2 = fl(2, rnd, -100, 100), fl(2, rnd, -100, 100)
        add("_ZN7cocos2d4Vec26rotateERKS0_f", ["B0", "B1", f2u(ang)], [v2, p2])
        add("_ZN7cocos2d4Vec39normalizeEv", ["B0"], [a3])
        add("_ZN7cocos2d4Vec29normalizeEv", ["B0"], [v2])
        add("_ZNK7cocos2d4Mat49decomposeEPNS_4Vec3EPNS_10QuaternionES2_", ["B0", "B1", "B2", "B3"],
            [m1, bytes(12), bytes(16), bytes(12)])
    open(out, "w").write("\n".join(lines) + "\n")
    print(f"{len(lines)} calls -> {out}")


if __name__ == "__main__":
    main()
