"""ELF32 ARM (armeabi-v7a) loader: image layout, relocations, imports, functions and ARM/Thumb mode.

Functions come from the symbol tables (Android libraries usually keep .symtab; .dynsym always has the
exported ones) and from .ARM.exidx unwind entries. Mapping symbols ($a/$t/$d) say which bytes are ARM
code, Thumb code or data (literal pools, jump tables), so the lifter never decodes data as code.
"""
import bisect, struct
from dataclasses import dataclass, field
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_ARM_ABS32, R_ARM_REL32, R_ARM_GLOB_DAT, R_ARM_JUMP_SLOT, R_ARM_RELATIVE = 2, 3, 21, 22, 23


@dataclass
class Image:
    path: str
    size: int = 0
    segments: list = field(default_factory=list)   # (vaddr, file bytes, memsz)
    relocs: list = field(default_factory=list)     # (type, offset, symname|None, symvalue)
    imports: list = field(default_factory=list)
    exports: dict = field(default_factory=dict)    # name -> value (functions keep the Thumb bit)
    funcs: dict = field(default_factory=dict)      # start -> (end or 0, thumb)
    maps: list = field(default_factory=list)       # sorted (addr, 'a'|'t'|'d')
    init_array: list = field(default_factory=list)
    text: tuple = (0, 0)
    plt: dict = field(default_factory=dict)        # PLT stub address -> (symbol name, defined value or 0)
    plt_range: tuple = (0, 0)
    elf: object = None

    def read(self, addr, n):
        for va, data, mem in self.segments:
            if va <= addr < va + len(data):
                return data[addr - va: addr - va + n]
        return b"\0" * n

    def u32(self, addr):
        return struct.unpack("<I", self.read(addr, 4).ljust(4, b"\0"))[0]

    def kind_at(self, addr):
        """Mapping-symbol kind covering addr ('a', 't', 'd') or None."""
        i = bisect.bisect_right(self._map_addrs, addr) - 1
        return self.maps[i][1] if i >= 0 else None

    def regions(self, start, end, thumb):
        """Code regions [(lo, hi, thumb)] inside [start, end), skipping data islands."""
        out = []
        i = bisect.bisect_right(self._map_addrs, start) - 1
        cur = self.maps[i][1] if i >= 0 else ("t" if thumb else "a")
        if cur not in ("a", "t", "d"):
            cur = "t" if thumb else "a"
        pos = start
        j = bisect.bisect_right(self._map_addrs, start)
        while pos < end:
            nxt = self.maps[j][0] if j < len(self.maps) and self.maps[j][0] < end else end
            if cur in ("a", "t") and nxt > pos:
                out.append((pos, nxt, cur == "t"))
            if nxt >= end:
                break
            pos, cur = nxt, self.maps[j][1]
            j += 1
        return out


def load(path) -> Image:
    f = ELFFile(open(path, "rb"))
    assert f["e_machine"] == "EM_ARM", "only 32-bit ARM ELF supported here"
    img = Image(path=path, elf=f)
    top = 0
    for seg in f.iter_segments():
        if seg["p_type"] == "PT_LOAD":
            img.segments.append((seg["p_vaddr"], seg.data(), seg["p_memsz"]))
            top = max(top, seg["p_vaddr"] + seg["p_memsz"])
    img.size = (top + 0xFFFF) & ~0xFFFF

    dynsym = f.get_section_by_name(".dynsym")
    imports = set()
    for s in dynsym.iter_symbols():
        if not s.name:
            continue
        if s["st_shndx"] == "SHN_UNDEF":
            imports.add(s.name)
        elif s["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT", "STT_GNU_IFUNC"):
            img.exports[s.name] = s["st_value"]
    img.imports = sorted(imports)

    relplt_off = set()
    for sec in f.iter_sections():
        if isinstance(sec, RelocationSection):
            for r in sec.iter_relocations():
                name, val = None, 0
                if r["r_info_sym"]:
                    sym = dynsym.get_symbol(r["r_info_sym"])
                    name = sym.name
                    val = 0 if sym["st_shndx"] == "SHN_UNDEF" else sym["st_value"]
                img.relocs.append((r["r_info_type"], r["r_offset"], name, val))

    # Functions and mapping symbols.
    maps = []
    for tab in (f.get_section_by_name(".symtab"), dynsym):
        if not tab:
            continue
        for s in tab.iter_symbols():
            if s["st_shndx"] in ("SHN_UNDEF", "SHN_ABS"):
                continue
            n = s.name
            if n.startswith("$a") or n.startswith("$t") or n.startswith("$d"):
                if n[:2] in ("$a", "$t", "$d") and (len(n) == 2 or n[2] == "."):
                    maps.append((s["st_value"], n[1]))
                continue
            if s["st_info"]["type"] in ("STT_FUNC", "STT_GNU_IFUNC") and s["st_value"]:
                v = s["st_value"]
                start, thumb = v & ~1, bool(v & 1)
                end = start + s["st_size"] if s["st_size"] else 0
                old = img.funcs.get(start)
                if not old or (end and (not old[0] or end > old[0])):
                    img.funcs[start] = (end, thumb)
    img.maps = sorted(set(maps))
    img._map_addrs = [a for a, _ in img.maps]

    # .ARM.exidx: one entry per function (prel31 start address), covers stripped libraries too.
    ex = f.get_section_by_name(".ARM.exidx")
    if ex:
        raw, base = ex.data(), ex["sh_addr"]
        for k in range(0, len(raw) - 7, 8):
            w = struct.unpack_from("<I", raw, k)[0]
            off = (w & 0x7FFFFFFF) | (0x80000000 if w & 0x40000000 else 0)
            start = (base + k + off) & 0xFFFFFFFF
            if start & 1:
                start &= ~1
            if start not in img.funcs:
                kd = img.kind_at(start)
                img.funcs[start] = (0, kd == "t")

    t = f.get_section_by_name(".text")
    img.text = (t["sh_addr"], t["sh_addr"] + t["sh_size"])

    # PLT: PLT0 (20 bytes) then one 12-byte stub per .rel.plt entry (gold/bfd short form).
    plt = f.get_section_by_name(".plt")
    relplt = f.get_section_by_name(".rel.plt")
    if plt and relplt:
        a = plt["sh_addr"]
        img.plt_range = (a, a + plt["sh_size"])
        n_rel = relplt.num_relocations()
        stride = 16 if plt["sh_size"] == 20 + 16 * n_rel else 12
        for n, r in enumerate(relplt.iter_relocations()):
            sym = dynsym.get_symbol(r["r_info_sym"])
            val = 0 if sym["st_shndx"] == "SHN_UNDEF" else sym["st_value"]
            img.plt[a + 20 + stride * n] = (sym.name, val, r["r_offset"])
        img.plt_stride = stride

    ia = f.get_section_by_name(".init_array")
    if ia:
        raw = ia.data()
        img.init_array = [v for v in struct.unpack(f"<{len(raw)//4}I", raw) if v not in (0, 0xFFFFFFFF)]
    return img
