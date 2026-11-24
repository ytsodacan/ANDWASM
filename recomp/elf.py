"""ELF32 i386 loader: image layout, relocations, imports, function list (from .eh_frame)."""
from dataclasses import dataclass, field
from elftools.elf.elffile import ELFFile
from elftools.elf.relocation import RelocationSection

R_386_32, R_386_PC32, R_386_GLOB_DAT, R_386_JMP_SLOT, R_386_RELATIVE = 1, 2, 6, 7, 8


@dataclass
class Image:
    path: str
    size: int = 0                       # bytes of address space (rounded)
    segments: list = field(default_factory=list)   # (vaddr, file bytes, memsz)
    relocs: list = field(default_factory=list)     # (type, offset, symname|None, symvalue)
    imports: list = field(default_factory=list)    # undefined symbol names (sorted)
    exports: dict = field(default_factory=dict)    # name -> vaddr (defined functions/objects)
    funcs: list = field(default_factory=list)      # sorted (start, end)
    init_array: list = field(default_factory=list) # vaddrs of constructors (pre-relocation offsets)
    text: tuple = (0, 0)
    elf: object = None

    def read(self, addr, n):
        for va, data, mem in self.segments:
            if va <= addr < va + len(data):
                return data[addr - va: addr - va + n]
        return b"\0" * n


def load(path) -> Image:
    f = ELFFile(open(path, "rb"))
    assert f["e_machine"] == "EM_386", "only i386 ELF supported"
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
        elif s["st_info"]["type"] in ("STT_FUNC", "STT_OBJECT"):
            img.exports[s.name] = s["st_value"]
    img.imports = sorted(imports)

    for sec in f.iter_sections():
        if isinstance(sec, RelocationSection):
            for r in sec.iter_relocations():
                name, val = None, 0
                if r["r_info_sym"]:
                    sym = dynsym.get_symbol(r["r_info_sym"])
                    name = sym.name
                    val = 0 if sym["st_shndx"] == "SHN_UNDEF" else sym["st_value"]
                img.relocs.append((r["r_info_type"], r["r_offset"], name, val))

    t = f.get_section_by_name(".text")
    img.text = (t["sh_addr"], t["sh_addr"] + t["sh_size"])
    ia = f.get_section_by_name(".init_array")
    if ia:
        import struct
        raw = ia.data()
        img.init_array = [v for v in struct.unpack(f"<{len(raw)//4}I", raw) if v not in (0, 0xFFFFFFFF)]

    fdes = [e for e in f.get_dwarf_info().EH_CFI_entries() if type(e).__name__ == "FDE"]
    spans = sorted({(e["initial_location"], e["initial_location"] + e["address_range"]) for e in fdes})
    # Functions without FDEs (PLT, thunks) are added later from call targets.
    img.funcs = spans
    return img
