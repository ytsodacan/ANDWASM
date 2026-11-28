#!/usr/bin/env python3
"""Recompile an armeabi-v7a Android .so into C for Emscripten (ARM/Thumb counterpart of recomp/build.py).

Usage: python3 -I recomp/arm/build_arm.py <lib.so> <out_dir> [--name libg]
Writes: <out>/gen/*.c (lifted functions), funcs.h, table.c (dispatch, relocations, imports),
        <name>.img (flat memory image), report.txt (unsupported instruction census).
"""
import argparse, bisect, collections, os, sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf_arm
from lift_arm import FuncLifter

CHUNK = 300
# Guest addresses that must never be lifted as a function (e.g. huge data-only symbols).
MAX_FUNC = int(os.environ.get("MAX_FUNC_BYTES", str(4 << 20)))


class Program:
    def __init__(self, image):
        self.image = image
        self.thumb = {}
        starts = {}
        lo, hi = image.text
        for s, (e, th) in image.funcs.items():
            if not (lo <= s < hi):
                continue
            starts[s] = e
            self.thumb[s] = th
        self.starts = starts
        self.split = set()
        self.finish()
        # Code addresses stored in data (relocated pointers): computed-goto tables, vtables, callbacks.
        lo_, hi_ = image.text
        self.code_ptrs = set()
        for typ, off, name, val in image.relocs:
            v = image.u32(off) if typ == elf_arm.R_ARM_RELATIVE else val
            if v and lo_ <= (v & ~1) < hi_:
                self.code_ptrs.add(v & ~1)
        # --stub NAME=VALUE (passed on as ANDWASM_STUBS): these functions just return VALUE in r0.
        self.wraps = set()
        wnames = {n for n in os.environ.get("ANDWASM_WRAPS", "").split(";") if n}
        if wnames:
            st_ = image.elf.get_section_by_name(".symtab") or image.elf.get_section_by_name(".dynsym")
            for sym in st_.iter_symbols():
                if sym.name in wnames and sym["st_value"]:
                    self.wraps.add(sym["st_value"] & ~1)
        self.stubs = {}
        want = dict(kv.split("=", 1) for kv in os.environ.get("ANDWASM_STUBS", "").split(";") if "=" in kv)
        if want:
            st = image.elf.get_section_by_name(".symtab") or image.elf.get_section_by_name(".dynsym")
            for sym in st.iter_symbols():
                if sym.name in want and sym["st_value"]:
                    self.stubs[sym["st_value"] & ~1] = int(want[sym.name], 0)

    def finish(self):
        lo, hi = self.image.text
        srt = sorted(self.starts)
        for a, b in zip(srt, srt[1:]):
            if not self.starts[a] or self.starts[a] > b:
                self.starts[a] = b
        if srt and not self.starts[srt[-1]]:
            self.starts[srt[-1]] = hi
        for a in srt:
            if self.starts[a] - a > MAX_FUNC:
                self.starts[a] = a + MAX_FUNC
        self.sorted = srt

    def covered(self, a):
        i = bisect.bisect_right(self.sorted, a) - 1
        return i >= 0 and self.sorted[i] <= a < self.starts[self.sorted[i]]

    def add_pointer_functions(self):
        lo, hi = self.image.text
        cands = {v for v in self.image.init_array}
        for typ, off, name, val in self.image.relocs:
            if typ == elf_arm.R_ARM_RELATIVE:
                cands.add(self.image.u32(off))
            elif val:
                cands.add(val)
        new, inside = 0, 0
        for v in cands:
            t = v & ~1
            if lo <= t < hi and t not in self.starts:
                if self.covered(t):
                    inside += 1
                    continue
                self.starts[t] = 0
                self.thumb[t] = bool(v & 1)
                new += 1
        self.finish()
        return new, inside

    def add_calls(self, targets):
        """Direct branch/call targets that are not function starts become entries. A target inside another
        function (shared tails like libgcc's soft-float normalisation) splits it; the first part then falls
        through into the second as a tail call, with the condition flags carried in g_nzcv."""
        lo, hi = self.image.text
        n = 0
        for t, th in targets:
            if lo <= t < hi and t not in self.starts and self.image.kind_at(t) != "d":
                if self.covered(t):
                    self.split.add(t)
                self.starts[t] = 0
                self.thumb[t] = th
                n += 1
        self.finish()
        return n

    VENEER = bytes.fromhex("7847c04600c09fe50ff08ce0")   # Thumb bx pc; nop; ARM ldr ip,[pc]; add pc,ip,pc; .word off

    def veneer(self, t):
        """Final target (Thumb bit kept) of a linker Thumb->ARM long-branch veneer at t, or None."""
        if self.image.read(t, 12) == self.VENEER:
            return (self.image.u32(t + 12) + t + 16) & 0xFFFFFFFF
        return None

    def call_expr(self, t, tail=True):
        v = self.veneer(t)
        if v is not None:
            t = v & ~1
        plt = self.image.plt.get(t)
        if plt:
            name, val, got = plt
            if name in ("setjmp", "_setjmp", "sigsetjmp"):
                # returns twice: run the host setjmp in this C frame; a longjmp lands here with g_r restored
                return "if(!setjmp(*rt_setjmp_slot(g_r[0]))) g_r[0]=0;"
            if val and (val & ~1) in self.starts:
                return f"F_{val & ~1:X}();"
            if not val:
                return f"imp_{name}();"
        if t in self.starts:
            if t in self.split:
                return f"g_nzcv=(n<<3)|(z<<2)|(c<<1)|v; g_lr0={'lr0' if tail else 'lr'}; F_{t:X}();"
            return f"F_{t:X}();"
        kind = self.image.kind_at(t)
        self.missing.add((t, kind == "t"))
        return f"rt_dispatch(B+0x{t:X}u{'|1u' if kind == 't' else ''});"


_PROG = None


def _init_worker(path, starts, thumb, split):
    global _PROG
    _PROG = Program(elf_arm.load(path))
    _PROG.starts = starts
    _PROG.thumb = thumb
    _PROG.split = split
    _PROG.sorted = sorted(starts)


def lift_chunk(chunk):
    prog = _PROG
    prog.missing = set()
    out, uns = [], []
    for s in chunk:
        if s in prog.stubs:
            out.append(f"void F_{s:X}(void){{ RT_TRACE(0x{s:X}u); g_r[0]=0x{prog.stubs[s] & 0xFFFFFFFF:X}u; }}")
            continue
        v = prog.veneer(s)
        if v is not None:
            c_ = prog.call_expr(s).strip(); pre_, _, last_ = c_.rpartition(" ")
            body_ = f"{pre_} MT({last_[:-1]});" if last_.endswith("();") and (last_.startswith("F_") or last_.startswith("imp_")) else c_
            out.append(f"void F_{s:X}(void){{ RT_TRACE(0x{s:X}u); uint32_t n=0,z=0,c=0,v=0,lr0=g_r[14]; (void)lr0; {body_} }}")
            continue
        fl = FuncLifter(prog, s, prog.starts[s], prog.thumb.get(s, False))
        if s in prog.wraps:
            # wrapped: the lifted body is renamed; the entry point calls the host hook, which can call it
            out.append(fl.lift().replace(f"void F_{s:X}(void){{", f"void F_{s:X}_orig(void){{", 1))
            out.append(f"void hook_{s:X}(void);\nvoid F_{s:X}(void){{ hook_{s:X}(); }}")
            uns += fl.unsupported
            continue
        out.append(fl.lift())
        uns += fl.unsupported
    return "\n\n".join(out), uns, prog.missing


def write_if_changed(path, text):
    try:
        if open(path).read() == text:
            return False
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)
    return True


def lift_all(a, prog, write=True, only=None):
    funcs = prog.sorted if only is None else sorted(only)
    chunks = [funcs[i:i + CHUNK] for i in range(0, len(funcs), CHUNK)]
    census = collections.Counter(); examples = {}; missing = set()
    with ProcessPoolExecutor(initializer=_init_worker, initargs=(a.so, prog.starts, prog.thumb, prog.split)) as ex:
        for n, (src, uns, miss) in enumerate(ex.map(lift_chunk, chunks)):
            if write:
                write_if_changed(f"{a.out}/gen/f{n:03d}.c", '#include "rt.h"\n#include "funcs.h"\n\n' + src + "\n")
            for ip, mn, why in uns:
                census[(mn, why)] += 1; examples.setdefault((mn, why), ip)
            missing |= miss
    return census, examples, missing, len(chunks)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so"); ap.add_argument("out"); ap.add_argument("--name", default="libg")
    ap.add_argument("--stub", action="append", default=[], help="SYMBOL=VALUE: replace a function with `return VALUE`")
    ap.add_argument("--wrap", action="append", default=[], help="SYMBOL: call host hook_<addr>() instead (it can call F_<addr>_orig)")
    a = ap.parse_args()
    if a.wrap:
        os.environ["ANDWASM_WRAPS"] = ";".join(a.wrap)
    if a.stub:
        os.environ["ANDWASM_STUBS"] = ";".join(a.stub)   # worker processes read it too
    img = elf_arm.load(a.so)
    prog = Program(img)
    prog.missing = set()
    ptrs, inside = prog.add_pointer_functions()
    print(f"symbol/exidx functions: {len(img.funcs)}; pointer-discovered: {ptrs} ({inside} inside existing functions)")
    if a.stub:
        print(f"stubbed functions: {len(prog.stubs)} of {len(a.stub)} requested")
    os.makedirs(f"{a.out}/gen", exist_ok=True)

    # Pass 1 finds direct-call targets that are not function starts; they become functions, then lift for real.
    census, examples, missing, nchunks = lift_all(a, prog, write=False)
    total_added, new = 0, None
    for rnd in range(12):    # newly found or newly shortened functions can reach further code: repeat until closed
        before = dict(prog.starts)
        added = prog.add_calls(missing)
        total_added += added
        new = {s_ for s_ in prog.starts if before.get(s_) != prog.starts[s_]}
        print(f"  discovery round {rnd + 1}: {added} new functions")
        if not new:
            break
        _, _, missing, _ = lift_all(a, prog, write=False, only=new)
    print(f"call-target functions added: {total_added} ({len(prog.split)} split existing functions)")
    census, examples, missing, nchunks = lift_all(a, prog, write=True)
    for f_ in os.listdir(f"{a.out}/gen"):
        if f_.startswith("f") and f_.endswith(".c") and int(f_[1:-2]) >= nchunks:
            os.remove(f"{a.out}/gen/{f_}")
    funcs = prog.sorted
    print(f"functions: {len(funcs)}; imports: {len(img.imports)}; unresolved call targets: {len(missing)}")

    mem = bytearray(img.size)
    for va, data, msz in img.segments:
        mem[va:va + len(data)] = data
    open(f"{a.out}/{a.name}.img", "wb").write(mem)

    with open(f"{a.out}/funcs.h", "w") as f:
        f.write("#pragma once\n" + "".join(f"void F_{s:X}(void);\n" for s in funcs))
        f.write("".join(f"void F_{s:X}_orig(void);\n" for s in sorted(prog.wraps)))
        f.write("".join(f"void imp_{n}(void);\n" for n in img.imports))

    imports = img.imports
    imp_idx = {n: i for i, n in enumerate(imports)}
    with open(f"{a.out}/table.c", "w") as f:
        f.write('#include "rt.h"\n#include "funcs.h"\n\n')
        f.write(f"const uint32_t IMAGE_SIZE=0x{img.size:X}u;\nconst char IMAGE_NAME[]=\"{a.name}.img\";\n")
        f.write(f"const uint32_t FUNC_COUNT={len(funcs)};\n")
        f.write("const uint32_t FUNC_ADDR[]={" + ",".join(f"0x{s:X}u" for s in funcs) + "};\n")
        f.write("void (*const FUNC_PTR[])(void)={" + ",".join(f"F_{s:X}" for s in funcs) + "};\n")
        rel = []
        for typ, off, name, val in img.relocs:
            if name is not None and name in imp_idx and val == 0:
                rel.append(f"{{{typ},0x{off:X}u,0,{imp_idx[name]}}}")
            else:
                rel.append(f"{{{typ},0x{off:X}u,0x{val:X}u,-1}}")
        f.write(f"const uint32_t RELOC_COUNT={len(rel)};\nconst reloc_t RELOCS[]={{" + ",".join(rel) + "};\n")
        f.write(f"const uint32_t INIT_COUNT={len(img.init_array)};\n")
        f.write("const uint32_t INIT_ARRAY[]={" + (",".join(f"0x{v:X}u" for v in img.init_array) or "0") + "};\n")
        f.write(f"const uint32_t IMPORT_COUNT={len(imports)};\n")
        f.write("const char *const IMPORT_NAME[]={" + ",".join(f'"{n}"' for n in imports) + "};\n")
        for n in imports:
            f.write(f'__attribute__((weak)) void imp_{n}(void){{ rt_missing_import("{n}"); }}\n')
        f.write("void (*const IMPORT_FN[])(void)={" + ",".join(f"imp_{n}" for n in imports) + "};\n")
        exp = sorted(img.exports.items())
        f.write(f"const uint32_t EXPORT_COUNT={len(exp)};\n")
        f.write("const export_t EXPORTS[]={" + ",".join(f'{{"{k}",0x{v:X}u}}' for k, v in exp) + "};\n")

    total = sum(census.values())
    rep = [f"functions {len(funcs)}", f"unsupported sites {total}"]
    rep += [f"{c:6d}  {mn:12s} {why:28s} e.g. 0x{examples[(mn, why)]:X}" for (mn, why), c in census.most_common()]
    open(f"{a.out}/report.txt", "w").write("\n".join(rep) + "\n")
    print("\n".join(rep[:60]))


if __name__ == "__main__":
    main()
