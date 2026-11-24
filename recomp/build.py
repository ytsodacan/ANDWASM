#!/usr/bin/env python3
"""Recompile an i386 Android .so into C for Emscripten.

Usage: python3 -I recomp/build.py <lib.so> <out_dir> [--name libg]
Writes: <out>/gen/*.c (lifted functions), funcs.h, table.c (dispatch + relocs + imports),
        <name>.img (flat memory image), report.txt (unsupported instruction census).
"""
import argparse, bisect, collections, os, sys
from concurrent.futures import ProcessPoolExecutor

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import elf
from lift import FuncLifter

CHUNK = 400  # functions per .c file


class Program:
    def __init__(self, image):
        self.image = image
        img = image
        starts = {}
        for s, e in img.funcs:
            if s not in starts or e > starts[s]:
                starts[s] = e
        plt = img.elf.get_section_by_name(".plt")
        if plt:
            a, z = plt["sh_addr"], plt["sh_addr"] + plt["sh_size"]
            for x in range(a + 16, z, 16):
                starts[x] = x + 16
        srt = sorted(starts)
        for a, b in zip(srt, srt[1:]):        # functions never overlap: cap at the next start
            starts[a] = min(starts[a], b)
        self.starts = starts
        self.sorted = srt
        self.got = img.elf.get_section_by_name(".got.plt")["sh_addr"]
        # PLT stubs by imported symbol name (PLT entry n+1 <-> n-th .rel.plt relocation)
        self.plt_by_name = {}
        relplt = img.elf.get_section_by_name(".rel.plt")
        if plt and relplt:
            dynsym = img.elf.get_section_by_name(".dynsym")
            for n, r in enumerate(relplt.iter_relocations()):
                self.plt_by_name[dynsym.get_symbol(r["r_info_sym"]).name] = plt["sh_addr"] + 16 * (n + 1)
        self.setjmp_plts = {self.plt_by_name[k] for k in ("setjmp", "_setjmp", "sigsetjmp") if k in self.plt_by_name}

    def add_gap_functions(self):
        """Direct call targets that are not covered by any FDE become functions (until the next start)."""
        import iced_x86 as I
        lo, hi = self.image.text
        data = self.image.read(lo, hi - lo)
        new = set()
        for ins in I.Decoder(32, data, ip=lo):
            if ins.mnemonic == I.Mnemonic.CALL and ins.op_kind(0) == I.OpKind.NEAR_BRANCH32:
                t = ins.near_branch_target
                if lo <= t < hi and t != ins.next_ip and t not in self.starts and not self.covered(t):
                    new.add(t)
        for t in sorted(new):
            i = bisect.bisect_right(self.sorted, t)
            end = self.sorted[i] if i < len(self.sorted) else hi
            self.starts[t] = end
        self.sorted = sorted(self.starts)
        return len(new)

    def add_pointer_functions(self):
        """Code addresses stored in data (init_array, vtables, callbacks) — taken from relocations."""
        lo, hi = self.image.text
        cands = set(self.image.init_array)
        for typ, off, name, val in self.image.relocs:
            if typ == elf.R_386_RELATIVE:
                cands.add(int.from_bytes(self.image.read(off, 4), "little"))
            elif val:
                cands.add(val)
        new = [t for t in cands if lo <= t < hi and t not in self.starts]
        inside = [t for t in new if self.covered(t)]
        for t in new:
            self.starts[t] = 0
        srt = sorted(self.starts)
        for a, b in zip(srt, srt[1:]):
            if self.starts[a] == 0 or self.starts[a] > b:
                self.starts[a] = b
        if self.starts[srt[-1]] == 0:
            self.starts[srt[-1]] = hi
        self.sorted = srt
        return len(new), len(inside)

    def covered(self, a):
        i = bisect.bisect_right(self.sorted, a) - 1
        return i >= 0 and self.sorted[i] <= a < self.starts[self.sorted[i]]

    def call_expr(self, t):
        if t in self.starts:
            return f"F_{t:X}();"
        return f"rt_dispatch(B+0x{t:X}u);"


_PROG = None


def _init_worker(path, starts):
    global _PROG
    _PROG = Program(elf.load(path))
    _PROG.starts = starts
    _PROG.sorted = sorted(starts)


def lift_chunk(chunk):
    prog = _PROG
    out, uns = [], []
    for s in chunk:
        fl = FuncLifter(prog, s, prog.starts[s])
        out.append(fl.lift())
        uns += fl.unsupported
    return "\n\n".join(out), uns


def write_if_changed(path, text):
    """Keep mtimes stable so make only recompiles chunks whose code actually changed."""
    try:
        if open(path).read() == text:
            return False
    except FileNotFoundError:
        pass
    with open(path, "w") as f:
        f.write(text)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("so"); ap.add_argument("out"); ap.add_argument("--name", default="libg")
    a = ap.parse_args()
    img = elf.load(a.so)
    prog = Program(img)
    gaps = prog.add_gap_functions()
    ptrs, inside = prog.add_pointer_functions()
    print(f"pointer-discovered functions: {ptrs} ({inside} split an existing FDE range)")
    funcs = prog.sorted
    os.makedirs(f"{a.out}/gen", exist_ok=True)
    print(f"functions: {len(funcs)} (incl. {gaps} gap functions); imports: {len(img.imports)}")

    # Flat image
    mem = bytearray(img.size)
    for va, data, msz in img.segments:
        mem[va:va + len(data)] = data
    open(f"{a.out}/{a.name}.img", "wb").write(mem)

    # Lift in parallel
    chunks = [funcs[i:i + CHUNK] for i in range(0, len(funcs), CHUNK)]
    census = collections.Counter(); examples = {}
    with ProcessPoolExecutor(initializer=_init_worker, initargs=(a.so, prog.starts)) as ex:
        for n, (src, uns) in enumerate(ex.map(lift_chunk, chunks)):
            write_if_changed(f"{a.out}/gen/f{n:03d}.c", '#include "rt.h"\n#include "funcs.h"\n\n' + src + "\n")
            for ip, mn, why in uns:
                census[(mn, why)] += 1; examples.setdefault((mn, why), ip)

    with open(f"{a.out}/funcs.h", "w") as f:
        f.write("#pragma once\n" + "".join(f"void F_{s:X}(void);\n" for s in funcs))

    # Dispatch table, relocations, imports
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

    rep = [f"functions {len(funcs)}", f"unsupported sites {sum(census.values())}"]
    rep += [f"{c:6d}  {mn:12s} {why:20s} e.g. 0x{examples[(mn, why)]:X}" for (mn, why), c in census.most_common()]
    open(f"{a.out}/report.txt", "w").write("\n".join(rep) + "\n")
    print("\n".join(rep[:40]))


if __name__ == "__main__":
    main()
