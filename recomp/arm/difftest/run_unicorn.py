#!/usr/bin/env python3
"""Reference runner: execute the calls from gen_tests.py on a real ARM CPU model (Unicorn, Cortex-A15 with
VFP/NEON) and print `r0 r1 <buffers>` per call.

Usage: run_unicorn.py <lib.so> <tests.txt> <out.txt>
Imports are emulated in Python (math functions, memcpy/memset); other imports stop the call with an error.
"""
import math
import struct
import sys

from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection
from unicorn import Uc, UcError, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE
from unicorn.arm_const import *

UB = 0x10000000          # image base
IMP = 0x0F000000         # import stubs (16 bytes each)
DATA_IMP = 0x0F800000    # data imports
STACK = 0x0E000000       # stack region (1 MB)
BUF = 0x0D000000         # call buffers (4 KB each)
RET = 0x0C000000         # magic return address


def f32(x):
    try:
        return struct.unpack("<f", struct.pack("<f", x))[0]
    except OverflowError:
        return math.copysign(float("inf"), x)


def u2f(u):
    return struct.unpack("<f", struct.pack("<I", u & 0xFFFFFFFF))[0]


def f2u(x):
    return struct.unpack("<I", struct.pack("<f", f32(x)))[0]


def q2d(lo, hi):
    return struct.unpack("<d", struct.pack("<II", lo & 0xFFFFFFFF, hi & 0xFFFFFFFF))[0]


def d2w(x):
    return struct.unpack("<II", struct.pack("<d", x))


def safe(fn, *a):
    try:
        return fn(*a)
    except (ValueError, OverflowError, ZeroDivisionError):
        return float("nan")


FLOAT1 = {"sinf": math.sin, "cosf": math.cos, "tanf": math.tan, "sqrtf": math.sqrt, "acosf": math.acos,
          "asinf": math.asin, "atanf": math.atan, "floorf": math.floor, "ceilf": math.ceil, "expf": math.exp,
          "logf": math.log, "roundf": lambda x: math.floor(x + 0.5) if x >= 0 else -math.floor(-x + 0.5), "fabsf": abs}
FLOAT2 = {"atan2f": math.atan2, "fmodf": math.fmod, "powf": math.pow}
DOUBLE1 = {"sin": math.sin, "cos": math.cos, "tan": math.tan, "sqrt": math.sqrt, "acos": math.acos, "asin": math.asin,
           "atan": math.atan, "floor": math.floor, "ceil": math.ceil, "exp": math.exp, "log": math.log}
DOUBLE2 = {"atan2": math.atan2, "fmod": math.fmod, "pow": math.pow}


def main():
    so, tests, out = sys.argv[1], sys.argv[2], sys.argv[3]
    elf = ELFFile(open(so, "rb"))
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    try:
        from unicorn.arm_const import UC_CPU_ARM_CORTEX_A15
        uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A15)
    except Exception:
        pass
    top = 0
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD":
            top = max(top, seg["p_vaddr"] + seg["p_memsz"])
    size = (top + 0xFFFF) & ~0xFFFF
    uc.mem_map(UB, size)
    for seg in elf.iter_segments():
        if seg["p_type"] == "PT_LOAD":
            uc.mem_write(UB + seg["p_vaddr"], seg.data())
    for base, sz in ((IMP, 0x100000), (DATA_IMP, 0x100000), (STACK, 0x100000), (BUF, 0x100000), (RET, 0x1000)):
        uc.mem_map(base, sz)

    dynsym = elf.get_section_by_name(".dynsym")
    imports, imp_addr, data_next = {}, {}, [DATA_IMP]

    def import_addr(name):
        if name in imp_addr:
            return imp_addr[name]
        if name in ("__stack_chk_guard",):
            a = data_next[0]; data_next[0] += 16
            uc.mem_write(a, struct.pack("<I", 0x2B1D5EED))
        elif name == "__sF" or name.startswith("SL_IID"):
            a = data_next[0]; data_next[0] += 256
        else:
            a = IMP + 16 * len(imports)
            imports[a] = name
        imp_addr[name] = a
        return a

    for sec in elf.iter_sections():
        if not isinstance(sec, RelocationSection):
            continue
        for r in sec.iter_relocations():
            t, off = r["r_info_type"], r["r_offset"]
            p = UB + off
            cur = struct.unpack("<I", uc.mem_read(p, 4))[0]
            sym = dynsym.get_symbol(r["r_info_sym"]) if r["r_info_sym"] else None
            if sym is not None:
                s = import_addr(sym.name) if sym["st_shndx"] == "SHN_UNDEF" else UB + sym["st_value"]
            if t == 23:
                uc.mem_write(p, struct.pack("<I", (cur + UB) & 0xFFFFFFFF))
            elif t == 2 and sym is not None:
                uc.mem_write(p, struct.pack("<I", (cur + s) & 0xFFFFFFFF))
            elif t in (21, 22) and sym is not None:
                uc.mem_write(p, struct.pack("<I", s & 0xFFFFFFFF))

    # enable VFP/NEON
    uc.reg_write(UC_ARM_REG_C1_C0_2, uc.reg_read(UC_ARM_REG_C1_C0_2) | (0xF << 20))
    uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)

    state = {"err": None}

    def on_import(u, addr, size, _):
        name = imports.get(addr & ~1)
        if name is None:
            return
        r = [u.reg_read(x) for x in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)]
        if name in FLOAT1:
            u.reg_write(UC_ARM_REG_R0, f2u(safe(FLOAT1[name], u2f(r[0]))))
        elif name in FLOAT2:
            u.reg_write(UC_ARM_REG_R0, f2u(safe(FLOAT2[name], u2f(r[0]), u2f(r[1]))))
        elif name in DOUBLE1:
            lo, hi = d2w(safe(DOUBLE1[name], q2d(r[0], r[1]))); u.reg_write(UC_ARM_REG_R0, lo); u.reg_write(UC_ARM_REG_R1, hi)
        elif name in DOUBLE2:
            lo, hi = d2w(safe(DOUBLE2[name], q2d(r[0], r[1]), q2d(r[2], r[3]))); u.reg_write(UC_ARM_REG_R0, lo); u.reg_write(UC_ARM_REG_R1, hi)
        elif name == "sincosf":
            x = u2f(r[0]); u.mem_write(r[1], struct.pack("<I", f2u(math.sin(x)))); u.mem_write(r[2], struct.pack("<I", f2u(math.cos(x))))
        elif name == "sincos":
            x = q2d(r[0], r[1]); u.mem_write(r[2], struct.pack("<d", math.sin(x))); u.mem_write(r[3], struct.pack("<d", math.cos(x)))
        elif name in ("memcpy", "__aeabi_memcpy", "__aeabi_memcpy4", "__aeabi_memcpy8", "memmove", "__aeabi_memmove",
                      "__aeabi_memmove4", "__aeabi_memmove8"):
            if r[2]: u.mem_write(r[0], bytes(u.mem_read(r[1], r[2])))
        elif name in ("__aeabi_memset", "__aeabi_memset4", "__aeabi_memset8"):
            if r[1]: u.mem_write(r[0], bytes([r[2] & 0xFF]) * r[1])
        elif name == "memset":
            if r[2]: u.mem_write(r[0], bytes([r[1] & 0xFF]) * r[2])
        elif name in ("__aeabi_memclr", "__aeabi_memclr4", "__aeabi_memclr8"):
            if r[1]: u.mem_write(r[0], bytes(r[1]))
        else:
            state["err"] = f"import {name}"
            u.emu_stop()
            return
        lr = u.reg_read(UC_ARM_REG_LR)
        u.reg_write(UC_ARM_REG_PC, lr)

    uc.hook_add(UC_HOOK_CODE, on_import, begin=IMP, end=IMP + 0x100000)

    outs = []
    for line in open(tests):
        line = line.strip()
        if not line:
            continue
        head, args, bufs = [p.strip() for p in line.split("|")]
        addr = int(head.split()[0], 16)
        bufs = [] if bufs == "-" else [bytes.fromhex(b) for b in bufs.split()]
        words = []
        for a in args.split():
            words.append(BUF + 0x1000 * int(a[1:]) if a.startswith("B") else int(a, 16))
        for k, b in enumerate(bufs):
            uc.mem_write(BUF + 0x1000 * k, b + bytes(0x1000 - len(b)))
        sp = STACK + 0x100000 - 0x100
        stack_words = words[4:]
        sp -= 4 * len(stack_words); sp &= ~7
        for k, w in enumerate(stack_words):
            uc.mem_write(sp + 4 * k, struct.pack("<I", w & 0xFFFFFFFF))
        for k, reg in enumerate((UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3)):
            uc.reg_write(reg, words[k] & 0xFFFFFFFF if k < len(words) else 0)
        for reg in (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                    UC_ARM_REG_R10, UC_ARM_REG_R11, UC_ARM_REG_R12):
            uc.reg_write(reg, 0)
        uc.reg_write(UC_ARM_REG_SP, sp)
        uc.reg_write(UC_ARM_REG_LR, RET)
        state["err"] = None
        try:
            uc.emu_start(UB + addr, RET, timeout=5_000_000, count=50_000_000)
        except UcError as e:
            state["err"] = f"uc {e} pc={uc.reg_read(UC_ARM_REG_PC) - UB:x}"
        if state["err"]:
            outs.append(f"ERR {state['err']}")
            continue
        r0, r1 = uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)
        res = [f"{r0:08x}", f"{r1:08x}"] + [bytes(uc.mem_read(BUF + 0x1000 * k, len(b))).hex() for k, b in enumerate(bufs)]
        outs.append(" ".join(res))
    open(out, "w").write("\n".join(outs) + "\n")
    print(f"{len(outs)} results -> {out} ({sum(1 for o in outs if o.startswith('ERR'))} errors)")


if __name__ == "__main__":
    main()
