"""ARMv7-A / Thumb-2 (+VFPv3, common NEON) → C static lifter for ANDWASM.

Same model as the x86 lifter (recomp/lift.py): every guest function becomes `void F_<off>(void)`.
Core registers r0-r12, sp, lr are C locals synced to the thread-local array g_r[] around calls and
returns; flags n,z,c,v are locals. VFP/NEON registers live in the thread-local union V (s/d/q views
of the same 256 bytes). Guest addresses equal wasm addresses: the image is loaded at `B`, so every
PC-derived address becomes `B + off`. Code addresses keep the ARM convention: bit 0 set = Thumb.

Returns: a write to PC (bx/pop/ldr/ldm/mov) whose value equals LR on entry (`lr0`) is a C return;
any other indirect PC write is a jump-table jump (resolved in-function) or a tail call via rt_dispatch.
"""
import os
import capstone as C
from capstone import arm as A

ALIAS = {"sb": "r9", "sl": "r10", "fp": "r11", "ip": "r12", "r13": "sp", "r14": "lr", "r15": "pc"}
CORE = [f"r{i}" for i in range(13)] + ["sp", "lr"]
COND = {1: "z", 2: "!z", 3: "c", 4: "!c", 5: "n", 6: "!n", 7: "v", 8: "!v", 9: "(c&&!z)", 10: "(!c||z)",
        11: "(n==v)", 12: "(n!=v)", 13: "(!z&&n==v)", 14: "(z||n!=v)"}
SFT = {A.ARM_SFT_LSL: "LSL", A.ARM_SFT_LSR: "LSR", A.ARM_SFT_ASR: "ASR", A.ARM_SFT_ROR: "ROR",
       A.ARM_SFT_LSL_REG: "LSL", A.ARM_SFT_LSR_REG: "LSR", A.ARM_SFT_ASR_REG: "ASR", A.ARM_SFT_ROR_REG: "ROR"}
SFT_REG = {A.ARM_SFT_LSL_REG, A.ARM_SFT_LSR_REG, A.ARM_SFT_ASR_REG, A.ARM_SFT_ROR_REG, A.ARM_SFT_RRX_REG}
NAME_ALIAS = {"hint": "nop", "movs": "mov", "fconsts": "vmov", "fconstd": "vmov", "fmstat": "vmrs", "ldmia": "ldm", "stmia": "stm",
              "fldmdbx": "vldmdb", "fstmdbx": "vstmdb", "fldmiax": "vldmia", "fstmiax": "vstmia"}

TRACE_FUNCS = {int(x, 16) for x in os.environ.get("TRACE_FUNCS", "").split(",") if x}


class Unsupported(Exception):
    pass


_MD = {}


def disasm(code, addr, thumb):
    md = _MD.get(thumb)
    if md is None:
        md = C.Cs(C.CS_ARCH_ARM, (C.CS_MODE_THUMB if thumb else C.CS_MODE_ARM) | C.CS_MODE_V8 * 0)
        md.detail = True
        _MD[thumb] = md
    out, pos, step = [], 0, 2 if thumb else 4
    while pos < len(code):
        got = False
        for i in md.disasm(code[pos:], addr + pos):
            out.append(i); pos = i.address + i.size - addr; got = True
        if pos < len(code):
            out.append(("bad", addr + pos)); pos += step
    return out


def vreg(name):
    """'s5' -> ('s', 5); 'd17' -> ('d', 17); 'q3' -> ('q', 3)."""
    if name and name[0] in "sdq" and name[1:].isdigit():
        return name[0], int(name[1:])
    return None


class FuncLifter:
    def __init__(self, prog, start, end, thumb):
        self.p, self.start, self.end, self.thumb = prog, start, end, thumb
        self.unsupported = []

    # ------------------------------------------------------------------ operands
    def rn(self, i, reg):
        n = i.reg_name(reg)
        return ALIAS.get(n, n)

    def pcv(self, i, align=False):
        if self.cur_thumb:
            v = i.address + 4
            return (v & ~3) if align else v
        return i.address + 8

    def rget(self, i, reg, align=False):
        n = self.rn(i, reg)
        if n == "pc":
            return f"(B+0x{self.pcv(i, align):X}u)"
        if n not in CORE:
            raise Unsupported(f"reg {n}")
        return n

    def shifted(self, i, o, base, carry):
        """Apply operand shift to expression `base`. carry=True: update c (returns pre, expr)."""
        st, sv = o.shift.type, o.shift.value
        if not st:
            return "", base
        if st in (A.ARM_SFT_RRX, A.ARM_SFT_RRX_REG):
            if carry:
                return f"t3={base}; t4=(c<<31)|(t3>>1); c=t3&1;", "t4"
            return "", f"((c<<31)|(({base})>>1))"
        amt = (f"({self.rget(i, sv)}&0xFFu)" if st in SFT_REG else f"{sv}u")
        fn = SFT[st]
        if carry:
            return f"t4={fn}C({base},{amt},&c);", "t4"
        return "", f"{fn}({base},{amt})"

    def op2(self, i, o, carry=False, align=False, rot=None):
        """(pre, expr) for a flexible second operand. rot: ARM 'imm, #rotate' form's rotate operand."""
        if o.type == A.ARM_OP_IMM:
            v = o.imm & 0xFFFFFFFF
            if rot is not None and rot.type == A.ARM_OP_IMM and rot.imm & 31:
                r = rot.imm & 31
                v = ((v >> r) | (v << (32 - r))) & 0xFFFFFFFF
            pre = f"c={v >> 31};" if carry and v > 0xFF else ""
            return pre, f"0x{v:X}u"
        if o.type == A.ARM_OP_REG:
            return self.shifted(i, o, self.rget(i, o.reg, align), carry)
        raise Unsupported(f"op2 type {o.type}")

    def maddr(self, i, o):
        m = o.mem
        bn = self.rn(i, m.base)
        if bn == "pc":
            base_c = self.pcv(i, True)
            if not m.index:
                return f"(B+0x{(base_c + m.disp) & 0xFFFFFFFF:X}u)"
            b = f"(B+0x{base_c:X}u)"
        else:
            b = self.rget(i, m.base)
        e = b
        if m.index:
            x = self.rget(i, m.index)
            if m.lshift:
                x = f"({x}<<{m.lshift})"
            elif o.shift.type:
                _, x = self.shifted(i, o, x, False)
            e += f"-{x}" if o.subtracted else f"+{x}"
        if m.disp:
            e += f"+0x{m.disp & 0xFFFFFFFF:X}u" if m.disp > 0 else f"-0x{(-m.disp) & 0xFFFFFFFF:X}u"
        return f"(uint32_t)({e})"

    def wreg(self, i, reg, val):
        n = self.rn(i, reg)
        if n == "pc":
            return self.jump(i, val)
        if n not in CORE:
            raise Unsupported(f"wreg {n}")
        return f"{n}=(uint32_t)({val});"

    # ------------------------------------------------------------------ control flow
    def call(self, t, tail=True):
        """Direct call (tail=False: BL, lr already set) or tail transfer to guest address t (bit 0 = Thumb)."""
        return self.p.call_expr(t & ~1, tail)

    def ret_addr(self, i):
        return f"B+0x{i.address + i.size:X}u" + ("|1u" if self.cur_thumb else "")

    @staticmethod
    def after_call(ra):
        """A callee may return past us (pop {..,pc} of an outer frame, as libgcc helpers do): g_unwind holds
        the address being returned to; frames unwind until the one whose call site it is."""
        # also reload the condition flags: helpers such as __aeabi_cdcmple return their result in them
        return (f"if(g_unwind){{ if(g_unwind==({ra})) g_unwind=0; else return; }} "
                f"n=(g_nzcv>>3)&1; z=(g_nzcv>>2)&1; c=(g_nzcv>>1)&1; v=g_nzcv&1;")

    def jump(self, i, val):
        """Indirect PC write: return, in-function table jump, or tail call."""
        cases = self.cases
        sw = f"switch((t0&~1u)-B){{ {cases} default: break; }} " if cases else ""
        return f"t0=(uint32_t)({val}); {sw}SYNC_OUT; g_nzcv=(n<<3)|(z<<2)|(c<<1)|v; if(t0==lr0) return; MT_TAIL(t0);"

    def tailcall(self, c):
        """Turn a call statement into a guaranteed tail call (WebAssembly return_call), so control that
        loops between functions (split fragments, interpreter handlers) never grows the stack."""
        c = c.strip()
        if c.startswith("rt_dispatch(") and c.endswith(");"):
            return f"MT_DISPATCH({c[len('rt_dispatch('):-2]});"
        pre, _, last = c.rpartition(" ")
        if last.endswith("();") and (last.startswith("F_") or last.startswith("imp_")):
            return f"{pre} MT({last[:-1]});".strip()
        return f"{c} return;"

    def branch(self, t):
        if t in self.addrs:
            return f"goto L_{t:X};"
        return f"SYNC_OUT; {self.tailcall(self.call(t))}"

    # ------------------------------------------------------------------ main
    def lift(self):
        p = self.p
        insns = []          # (insn, thumb)
        for lo, hi, th in p.image.regions(self.start, self.end, self.thumb):
            for x in disasm(p.image.read(lo, hi - lo), lo, th):
                insns.append((x, th))
        self.addrs = {x.address for x, _ in insns if not isinstance(x, tuple)}
        targets = set()
        self.tables = {}    # address of table jump -> list of targets
        for n, (x, th) in enumerate(insns):
            if isinstance(x, tuple):
                continue
            self.cur_thumb = th
            nm = x.insn_name()
            if nm in ("b", "cbz", "cbnz"):
                t = x.operands[-1].imm & 0xFFFFFFFF
                if t in self.addrs:
                    targets.add(t)
            elif nm in ("tbb", "tbh") or (nm in ("add", "ldr", "mov") and x.operands and x.operands[0].type == A.ARM_OP_REG
                                          and self.rn(x, x.operands[0].reg) == "pc"):
                tt = self.jump_table(insns, n, x, nm)
                if tt:
                    self.tables[x.address] = tt
                    targets |= tt
        self.scan_code_pointers(insns)
        jt = set()
        for v in self.tables.values():
            jt |= v
        # Computed gotos (e.g. an interpreter's opcode table): code pointers in data that land inside this
        # function become labels, so an indirect jump to one is a local goto, not a nested call.
        cp = {a for a in getattr(p, "code_ptrs", ()) if self.start < a < self.end and a in self.addrs}
        jt |= cp
        targets |= cp
        self.cases = " ".join(f"case 0x{a:X}u: goto L_{a:X};" for a in sorted(jt))
        o = []
        hit = f" rt_trace_hit(0x{self.start:X}u);" if self.start in TRACE_FUNCS else ""
        o.append(f"void F_{self.start:X}(void){{ RT_TRACE(0x{self.start:X}u);{hit}")
        o.append(" uint32_t " + ",".join(f"{r}=g_r[{k}]" for k, r in enumerate(CORE)) + ";")
        o.append(" uint32_t lr0=lr; uint32_t n=0,z=0,c=0,v=0,fn=0,fz=0,fc=0,fv=0; uint32_t t0,t1,t2,t3,t4; uint64_t q0;"
                 " (void)t0;(void)t1;(void)t2;(void)t3;(void)t4;(void)q0;(void)fn;(void)fz;(void)fc;(void)fv;")
        if self.start in getattr(p, "split", ()):
            o.append(" n=(g_nzcv>>3)&1; z=(g_nzcv>>2)&1; c=(g_nzcv>>1)&1; v=g_nzcv&1; lr0=g_lr0;")
        for x, th in insns:
            if isinstance(x, tuple):
                o.append(f" /* undecodable {x[1]:X} */")
                continue
            self.cur_thumb = th
            if x.address in targets:
                o.append(f"L_{x.address:X}:;")
            try:
                s = self.insn(x)
                cc = x.cc
                if cc not in (0, 15) and x.insn_name() not in ("b", "it"):
                    s = f"if({COND[cc]}){{ {s} }}"
                o.append(" " + s)
            except Unsupported as e:
                nm = x.insn_name()
                self.unsupported.append((x.address, nm, str(e)))
                o.append(f" SYNC_OUT; rt_unsupported(0x{x.address:X}u); return; /* {x.mnemonic} {x.op_str} */")
        if self.end in p.starts:   # code continues into the next function: tail call it
            o.append(f" SYNC_OUT; {self.tailcall(p.call_expr(self.end))}\n}}")
        else:
            o.append(f" SYNC_OUT; rt_fallthrough(0x{self.start:X}u);\n}}")
        text = "\n".join(o)
        if "setjmp(" in text:   # musttail is not safe in functions that call setjmp: plain call + return
            text = text.replace("MT(", "NT(").replace("MT_DISPATCH(", "NT_DISPATCH(").replace("MT_TAIL(", "NT_TAIL(")
        return text

    def scan_code_pointers(self, insns):
        """Find code addresses built PC-relative (function pointers with no relocation):
        `ldr rX,[pc,#k]` + `add rX, pc` (literal + PC), `adr rX, l` and `add rX, pc, #imm`.
        Candidates go to p.missing so the build makes them function entries."""
        img, p = self.p.image, self.p
        lo, hi = img.text
        lit = {}
        def cand(v):
            t, th = v & ~1, bool(v & 1)
            k = img.kind_at(t)
            if lo <= t < hi and k == ("t" if th else "a") and t not in p.starts:
                p.missing.add((t, th))
        for x, th in insns:
            if isinstance(x, tuple):
                lit.clear(); continue
            self.cur_thumb = th
            nm, ops = x.insn_name(), x.operands
            try:
                if nm == "ldr" and len(ops) == 2 and ops[1].type == A.ARM_OP_MEM and self.rn(x, ops[1].mem.base) == "pc" \
                        and not ops[1].mem.index and ops[0].type == A.ARM_OP_REG:
                    lit[self.rn(x, ops[0].reg)] = img.u32((self.pcv(x, True) + ops[1].mem.disp) & 0xFFFFFFFF)
                    continue
                if nm == "adr":
                    cand((self.pcv(x, True) + ops[1].imm) & 0xFFFFFFFF)
                elif nm == "add" and ops and ops[0].type == A.ARM_OP_REG:
                    regs = [self.rn(x, o.reg) if o.type == A.ARM_OP_REG else None for o in ops]
                    if "pc" in regs[1:]:
                        others = [r for r in regs[1:] if r and r != "pc"] or ([regs[0]] if len(ops) == 2 else [])
                        imms = [o.imm for o in ops[1:] if o.type == A.ARM_OP_IMM]
                        if imms and not others:
                            cand((self.pcv(x, True) + imms[0]) & 0xFFFFFFFF)
                        elif len(others) == 1 and others[0] in lit and not ops[-1].shift.type:
                            cand((lit[others[0]] + self.pcv(x)) & 0xFFFFFFFF)
                # any other write to a register forgets its literal
                if ops and ops[0].type == A.ARM_OP_REG:
                    lit.pop(self.rn(x, ops[0].reg), None)
                if nm in ("bl", "blx", "b", "bx", "pop", "ldm", "cbz", "cbnz"):
                    lit.clear()
            except Exception:
                lit.clear()

    def table_count(self, insns, n, reg):
        """Entries in a switch table: find `cmp reg, #k` shortly before (k+1 entries)."""
        for back in range(1, 8):
            if n - back < 0:
                break
            x, _ = insns[n - back]
            if isinstance(x, tuple):
                break
            if x.insn_name() == "cmp" and len(x.operands) == 2 and x.operands[0].type == A.ARM_OP_REG \
                    and x.operands[0].reg == reg and x.operands[1].type == A.ARM_OP_IMM:
                return x.operands[1].imm + 1
        return None

    def jump_table(self, insns, n, x, nm):
        img = self.p.image
        if nm in ("tbb", "tbh"):
            m = x.operands[0].mem
            cnt = self.table_count(insns, n, m.index) or 256
            base = x.address + 4
            out = set()
            for k in range(cnt):
                e = img.read(base + k, 1)[0] if nm == "tbb" else int.from_bytes(img.read(base + 2 * k, 2), "little")
                t = base + 2 * e
                if t not in self.addrs:
                    if cnt == 256: break
                    continue
                out.add(t)
            return out
        ops = x.operands
        if nm == "add" and len(ops) == 3 and ops[1].type == A.ARM_OP_REG and self.rn(x, ops[1].reg) == "pc" \
                and ops[2].type == A.ARM_OP_REG:
            # ARM: addls pc, pc, rX, lsl #2 ; b default ; b case0 ; ...
            cnt = self.table_count(insns, n, ops[2].reg)
            if cnt is None:
                return None
            sh = ops[2].shift.value if ops[2].shift.type == A.ARM_SFT_LSL else 0
            base = self.pcv(x)
            return {base + (k << sh) for k in range(cnt) if base + (k << sh) in self.addrs}
        if nm == "ldr" and len(ops) == 2 and ops[1].type == A.ARM_OP_MEM and self.rn(x, ops[1].mem.base) == "pc" \
                and ops[1].mem.index:
            cnt = self.table_count(insns, n, ops[1].mem.index)
            if cnt is None:
                return None
            base = self.pcv(x, True)
            out = set()
            for k in range(cnt):
                t = img.u32(base + 4 * k) & ~1
                if t in self.addrs:
                    out.add(t)
            return out
        return None

    # ------------------------------------------------------------------ instructions
    def insn(self, i):
        nm0 = i.insn_name()
        nm = NAME_ALIAS.get(nm0, nm0)
        ops = i.operands
        S = self.sets_flags(i, nm0)

        # ---- branches ----
        if nm == "b":
            t = ops[0].imm & 0xFFFFFFFF
            s = self.branch(t)
            if i.cc not in (0, 15):
                return f"if({COND[i.cc]}){{ {s} }}"
            return s
        if nm in ("cbz", "cbnz"):
            r = self.rget(i, ops[0].reg)
            t = ops[1].imm & 0xFFFFFFFF
            return f"if({r}{'==' if nm == 'cbz' else '!='}0){{ {self.branch(t)} }}"
        if nm in ("bl", "blx") and ops[0].type == A.ARM_OP_IMM:
            t = ops[0].imm & 0xFFFFFFFF
            if nm == "blx" and self.cur_thumb:
                t &= ~3     # Thumb BLX imm targets ARM code (word aligned)
            ra = self.ret_addr(i)
            return f"lr={ra}; SYNC_OUT; {self.call(t, False)} {self.after_call(ra)} SYNC_IN;"
        if nm == "blx":
            r = self.rget(i, ops[0].reg)
            ra = self.ret_addr(i)
            return f"t0={r}; lr={ra}; SYNC_OUT; rt_dispatch(t0); {self.after_call(ra)} SYNC_IN;"
        if nm in ("bx", "bxj"):
            return self.jump(i, self.rget(i, ops[0].reg))
        if nm in ("tbb", "tbh"):
            m = ops[0].mem
            base = i.address + 4
            x = self.rget(i, m.index)
            ld = f"M8(B+0x{base:X}u+{x})" if nm == "tbb" else f"M16(B+0x{base:X}u+({x}<<1))"
            return self.jump(i, f"B+0x{base:X}u+2u*{ld}")
        if nm == "it":
            return ";"

        # ---- moves / data processing ----
        if nm in ("mov", "mvn"):
            if len(ops) == 2:
                pre, e = self.op2(i, ops[1], carry=S)
            elif len(ops) == 3 and ops[1].type == A.ARM_OP_IMM and ops[2].type == A.ARM_OP_IMM:
                pre, e = self.op2(i, ops[1], carry=S, rot=ops[2])
            else:
                raise Unsupported("mov form")
            if nm == "mvn":
                e = f"(~{e})"
            if S:
                return f"{pre} t2={e}; n=t2>>31; z=t2==0; {self.wreg(i, ops[0].reg, 't2')}"
            return self.wreg(i, ops[0].reg, e)
        if nm == "movt":
            d = self.rget(i, ops[0].reg)
            return f"{d}=({d}&0xFFFFu)|(0x{ops[1].imm & 0xFFFF:X}u<<16);"
        if nm == "adr":
            return self.wreg(i, ops[0].reg, f"B+0x{(self.pcv(i, True) + ops[1].imm) & 0xFFFFFFFF:X}u")
        if nm in ("lsl", "lsr", "asr", "ror", "rrx") and len(ops) == 2 and ops[1].type == A.ARM_OP_REG and ops[1].shift.type:
            # ARM-mode form: capstone shows `lsl rd, rm, #n` / `lsl rd, rm, rs` as rd + (rm with the shift attached)
            pre, e = self.op2(i, ops[1], carry=S)
            if S:
                return f"{pre} t2={e}; n=t2>>31; z=t2==0; {self.wreg(i, ops[0].reg, 't2')}"
            return self.wreg(i, ops[0].reg, e)
        if nm in ("lsl", "lsr", "asr", "ror", "rrx"):
            d = ops[0]
            if nm == "rrx":
                a = self.rget(i, ops[1].reg)
                e = f"((c<<31)|({a}>>1))"
                if S:
                    return f"t3={a}; t2=(c<<31)|(t3>>1); c=t3&1; n=t2>>31; z=t2==0; {self.wreg(i, d.reg, 't2')}"
                return self.wreg(i, d.reg, e)
            if len(ops) == 2:
                a, amt_o = self.rget(i, d.reg), ops[1]
            else:
                a, amt_o = self.rget(i, ops[1].reg), ops[2]
            amt = f"{amt_o.imm}u" if amt_o.type == A.ARM_OP_IMM else f"({self.rget(i, amt_o.reg)}&0xFFu)"
            fnm = nm.upper()
            if S:
                return f"t2={fnm}C({a},{amt},&c); n=t2>>31; z=t2==0; {self.wreg(i, d.reg, 't2')}"
            return self.wreg(i, d.reg, f"{fnm}({a},{amt})")
        if nm in ("add", "adc", "sub", "sbc", "rsb", "rsc", "and", "orr", "eor", "bic", "orn",
                  "cmp", "cmn", "tst", "teq", "addw", "subw"):
            return self.dp(i, nm, ops, S)

        # ---- multiply / divide ----
        if nm in ("mul", "mla", "mls"):
            a, b = self.rget(i, ops[1].reg), self.rget(i, ops[2].reg) if len(ops) > 2 else self.rget(i, ops[0].reg)
            if len(ops) == 2:
                a, b = self.rget(i, ops[0].reg), self.rget(i, ops[1].reg)
            e = f"({a}*{b})"
            if nm == "mla": e = f"({e}+{self.rget(i, ops[3].reg)})"
            if nm == "mls": e = f"({self.rget(i, ops[3].reg)}-{e})"
            if S:
                return f"t2={e}; n=t2>>31; z=t2==0; {self.wreg(i, ops[0].reg, 't2')}"
            return self.wreg(i, ops[0].reg, e)
        if nm in ("umull", "smull", "umlal", "smlal"):
            lo, hi = self.rget(i, ops[0].reg), self.rget(i, ops[1].reg)
            a, b = self.rget(i, ops[2].reg), self.rget(i, ops[3].reg)
            if nm[0] == "u":
                prod = f"(uint64_t){a}*(uint64_t){b}"
            else:
                prod = f"(uint64_t)((int64_t)(int32_t){a}*(int64_t)(int32_t){b})"
            if nm.endswith("lal"):
                prod = f"({prod}+(((uint64_t){hi}<<32)|{lo}))"
            s = f"q0={prod}; {lo}=(uint32_t)q0; {hi}=(uint32_t)(q0>>32);"
            if S: s += f" n=(uint32_t)(q0>>63); z=q0==0;"
            return s
        if nm in ("smmul", "smmla", "smmls"):
            a, b = self.rget(i, ops[1].reg), self.rget(i, ops[2].reg)
            p_ = f"((int64_t)(int32_t){a}*(int64_t)(int32_t){b})"
            if nm == "smmla": p_ = f"((((int64_t)(int32_t){self.rget(i, ops[3].reg)})<<32)+{p_})"
            if nm == "smmls": p_ = f"((((int64_t)(int32_t){self.rget(i, ops[3].reg)})<<32)-{p_})"
            return self.wreg(i, ops[0].reg, f"(uint32_t)((uint64_t){p_}>>32)")
        if nm in ("smulbb", "smulbt", "smultb", "smultt", "smlabb", "smlabt", "smlatb", "smlatt"):
            a, b = self.rget(i, ops[1].reg), self.rget(i, ops[2].reg)
            ha = f"(int32_t)(int16_t)({a}>>16)" if nm[4] == "t" else f"(int32_t)(int16_t){a}"
            hb = f"(int32_t)(int16_t)({b}>>16)" if nm[5] == "t" else f"(int32_t)(int16_t){b}"
            e = f"(uint32_t)({ha}*{hb})"
            if nm.startswith("smla"): e = f"({e}+{self.rget(i, ops[3].reg)})"
            return self.wreg(i, ops[0].reg, e)
        if nm in ("sdiv", "udiv"):
            a, b = self.rget(i, ops[1].reg), self.rget(i, ops[2].reg)
            if nm == "udiv":
                return self.wreg(i, ops[0].reg, f"({b}?{a}/{b}:0u)")
            return self.wreg(i, ops[0].reg, f"({b}?(uint32_t)(((int32_t){b}==-1)?(uint32_t)(0u-{a}):(uint32_t)((int32_t){a}/(int32_t){b})):0u)")

        # ---- bit ops ----
        if nm == "clz":
            a = self.rget(i, ops[1].reg)
            return self.wreg(i, ops[0].reg, f"({a}?(uint32_t)__builtin_clz({a}):32u)")
        if nm == "rbit":
            return self.wreg(i, ops[0].reg, f"rbit32({self.rget(i, ops[1].reg)})")
        if nm == "rev":
            return self.wreg(i, ops[0].reg, f"__builtin_bswap32({self.rget(i, ops[1].reg)})")
        if nm == "rev16":
            a = self.rget(i, ops[1].reg)
            return self.wreg(i, ops[0].reg, f"((({a}>>8)&0x00FF00FFu)|(({a}<<8)&0xFF00FF00u))")
        if nm == "revsh":
            a = self.rget(i, ops[1].reg)
            return self.wreg(i, ops[0].reg, f"(uint32_t)(int32_t)(int16_t)__builtin_bswap16((uint16_t){a})")
        if nm in ("uxtb", "uxth", "sxtb", "sxth", "uxtab", "uxtah", "sxtab", "sxtah", "uxtb16"):
            src = ops[-1]
            x = self.rget(i, src.reg)
            if src.shift.type and src.shift.value:
                x = f"ROR({x},{src.shift.value}u)"
            ext = {"uxtb": f"({x}&0xFFu)", "uxth": f"({x}&0xFFFFu)", "sxtb": f"(uint32_t)(int32_t)(int8_t){x}",
                   "sxth": f"(uint32_t)(int32_t)(int16_t){x}", "uxtb16": f"({x}&0x00FF00FFu)"}[nm.replace("a", "") if nm != "uxtb16" else nm]
            if "a" in nm[3:]:
                ext = f"({self.rget(i, ops[1].reg)}+{ext})"
            return self.wreg(i, ops[0].reg, ext)
        if nm in ("ubfx", "sbfx"):
            a, lsb, w = self.rget(i, ops[1].reg), ops[2].imm, ops[3].imm
            if nm == "ubfx":
                mask = (1 << w) - 1
                return self.wreg(i, ops[0].reg, f"(({a}>>{lsb})&0x{mask:X}u)")
            return self.wreg(i, ops[0].reg, f"(uint32_t)((int32_t)({a}<<{32 - lsb - w})>>{32 - w})")
        if nm == "bfi":
            d, a, lsb, w = self.rget(i, ops[0].reg), self.rget(i, ops[1].reg), ops[2].imm, ops[3].imm
            mask = ((1 << w) - 1) << lsb
            return f"{d}=({d}&0x{~mask & 0xFFFFFFFF:X}u)|(({a}<<{lsb})&0x{mask:X}u);"
        if nm == "bfc":
            d, lsb, w = self.rget(i, ops[0].reg), ops[1].imm, ops[2].imm
            mask = ((1 << w) - 1) << lsb
            return f"{d}&=0x{~mask & 0xFFFFFFFF:X}u;"
        if nm in ("usat", "ssat"):
            sat = ops[1].imm
            _, x = self.shifted(i, ops[2], self.rget(i, ops[2].reg), False)
            if nm == "usat":
                hi_ = (1 << sat) - 1
                return self.wreg(i, ops[0].reg, f"(((int32_t)({x}))<0?0u:((uint32_t)({x})>{hi_}u?{hi_}u:(uint32_t)({x})))")
            hi_, lo_ = (1 << (sat - 1)) - 1, -(1 << (sat - 1))
            return self.wreg(i, ops[0].reg, f"(uint32_t)(((int32_t)({x}))>{hi_}?{hi_}:(((int32_t)({x}))<({lo_})?({lo_}):(int32_t)({x})))")
        if nm in ("pkhbt", "pkhtb"):
            a = self.rget(i, ops[1].reg)
            _, b = self.shifted(i, ops[2], self.rget(i, ops[2].reg), False)
            if nm == "pkhbt":
                return self.wreg(i, ops[0].reg, f"(({a}&0xFFFFu)|(({b})&0xFFFF0000u))")
            return self.wreg(i, ops[0].reg, f"(({a}&0xFFFF0000u)|(({b})&0xFFFFu))")
        if nm == "umaal":
            lo, hi = self.rget(i, ops[0].reg), self.rget(i, ops[1].reg)
            a, b = self.rget(i, ops[2].reg), self.rget(i, ops[3].reg)
            return f"q0=(uint64_t){a}*(uint64_t){b}+(uint64_t){lo}+(uint64_t){hi}; {lo}=(uint32_t)q0; {hi}=(uint32_t)(q0>>32);"

        # ---- loads / stores ----
        r = self.mem(i, nm, ops)
        if r is not None:
            return r

        # ---- system ----
        if nm in ("nop", "dmb", "dsb", "isb", "yield", "pld", "pli", "pldw", "clrex", "wfe", "wfi", "sev"):
            return ";" if nm not in ("dmb", "dsb") else "__atomic_thread_fence(__ATOMIC_SEQ_CST);"
        if nm in ("udf", "bkpt", "trap"):
            return f"SYNC_OUT; rt_trap(0x{i.address:X}u); return;"
        if nm == "svc":
            return f"SYNC_OUT; rt_svc(0x{i.address:X}u); SYNC_IN;"
        if nm == "mrc":
            # mrc p15, 0, rX, c13, c0, 3 : TLS pointer (TPIDRURO)
            if "c13, c0, #3" in i.op_str or "c13, c0, 3" in i.op_str:
                for o_ in ops:
                    if o_.type == A.ARM_OP_REG:
                        return self.wreg(i, o_.reg, "rt_tls()")
            raise Unsupported(f"mrc {i.op_str}")
        if nm == "mrs":
            return self.wreg(i, ops[0].reg, "((n<<31)|(z<<30)|(c<<29)|(v<<28))")
        if nm == "msr":
            src = ops[-1]
            val = self.rget(i, src.reg) if src.type == A.ARM_OP_REG else f"0x{src.imm & 0xFFFFFFFF:X}u"
            return f"t0={val}; n=(t0>>31)&1; z=(t0>>30)&1; c=(t0>>29)&1; v=(t0>>28)&1;"

        r = self.vfp(i, nm, ops)
        if r is not None:
            return r
        raise Unsupported(nm)

    CONDS = ("eq", "ne", "hs", "cs", "lo", "cc", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")

    def sets_flags(self, i, nm0):
        """Whether an instruction writes the flags, from its mnemonic (`adds`, `sbcs.w`, `subsne`): capstone's
        update_flags is also true for plain `sbc`/`adc`, which must leave the flags alone."""
        if nm0 == "movs":
            return True
        base = i.mnemonic.split(".")[0]
        if i.cc not in (0, 15):
            for cn in self.CONDS:
                if base.endswith(cn):
                    base = base[:-2]
                    break
        return base == nm0 + "s"

    def dp(self, i, nm, ops, S):
        logical = nm in ("and", "orr", "eor", "bic", "orn", "tst", "teq")
        if nm in ("cmp", "cmn", "tst", "teq"):
            a = self.rget(i, ops[0].reg)
            pre, b = self.op2(i, ops[1], carry=logical, rot=ops[2] if len(ops) > 2 else None)
            dst = None
            S = True
        elif len(ops) == 2:
            dst = ops[0].reg
            a = self.rget(i, ops[0].reg)
            pre, b = self.op2(i, ops[1], carry=logical and S)
        else:
            dst = ops[0].reg
            imm_form = ops[2].type == A.ARM_OP_IMM
            a = self.rget(i, ops[1].reg, align=imm_form)
            pre, b = self.op2(i, ops[2], carry=logical and S, rot=ops[3] if len(ops) > 3 else None)
        base = nm.rstrip("w") if nm in ("addw", "subw") else nm
        if base in ("add", "cmn"):
            e = f"t0={a}; t1={b}; t2=t0+t1;"
            fl = "n=t2>>31; z=t2==0; c=t2<t0; v=((t0^t2)&(t1^t2))>>31;"
        elif base == "adc":
            e = f"t0={a}; t1={b}; q0=(uint64_t)t0+t1+c; t2=(uint32_t)q0;"
            fl = "n=t2>>31; z=t2==0; c=(uint32_t)(q0>>32); v=((t0^t2)&(t1^t2))>>31;"
        elif base in ("sub", "cmp"):
            e = f"t0={a}; t1={b}; t2=t0-t1;"
            fl = "n=t2>>31; z=t2==0; c=t0>=t1; v=((t0^t1)&(t0^t2))>>31;"
        elif base == "sbc":
            e = f"t0={a}; t1={b}; t2=t0-t1-(c^1u);"
            fl = "n=t2>>31; z=t2==0; c=(uint64_t)t0>=(uint64_t)t1+(c^1u); v=((t0^t1)&(t0^t2))>>31;"
        elif base == "rsb":
            e = f"t1={a}; t0={b}; t2=t0-t1;"
            fl = "n=t2>>31; z=t2==0; c=t0>=t1; v=((t0^t1)&(t0^t2))>>31;"
        elif base == "rsc":
            e = f"t1={a}; t0={b}; t2=t0-t1-(c^1u);"
            fl = "n=t2>>31; z=t2==0; c=(uint64_t)t0>=(uint64_t)t1+(c^1u); v=((t0^t1)&(t0^t2))>>31;"
        else:
            op = {"and": "&", "tst": "&", "orr": "|", "eor": "^", "teq": "^", "bic": "&~", "orn": "|~"}[base]
            e = f"t2={a}{op}({b});"
            fl = "n=t2>>31; z=t2==0;"
        s = f"{pre} {e}"
        if S:
            if base == "sbc" or base == "rsc":
                # carry uses the old c: compute flags before c changes (fl reads c^1u)
                s += f" {fl}"
            else:
                s += f" {fl}"
        if dst is not None:
            s += " " + self.wreg(i, dst, "t2")
        return s

    # ------------------------------------------------------------------ memory
    def mem(self, i, nm, ops):
        LD = {"ldr": ("M32", None), "ldrb": ("M8", None), "ldrh": ("M16", None), "ldrsb": ("M8", "int8_t"),
              "ldrsh": ("M16", "int16_t"), "ldrt": ("M32", None), "ldrbt": ("M8", None), "ldrht": ("M16", None)}
        ST = {"str": "W32", "strb": "W8", "strh": "W16", "strt": "W32", "strbt": "W8", "strht": "W16"}
        if nm in LD or nm in ST or nm in ("ldrd", "strd"):
            regs = [o for o in ops if o.type == A.ARM_OP_REG]
            mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
            extra = ops[-1] if ops[-1].type != A.ARM_OP_MEM else None
            post = i.post_index or (extra is not None and ops.index(extra) > ops.index(mo))
            basen = self.rn(i, mo.mem.base)
            if post:
                addr = "t1"
                b = self.rget(i, mo.mem.base)
                if extra.type == A.ARM_OP_IMM:
                    off = extra.imm
                    neg = extra.subtracted or off < 0
                    off = abs(off)
                    upd = f"{b}={b}{'-' if neg else '+'}0x{off:X}u;"
                else:
                    _, x = self.shifted(i, extra, self.rget(i, extra.reg), False)
                    upd = f"{b}={b}{'-' if extra.subtracted else '+'}{x};"
                pre_ = f"t1={b};"
            else:
                pre_ = f"t1={self.maddr(i, mo)};"
                upd = f"{self.rget(i, mo.mem.base)}=t1;" if i.writeback and basen != "pc" else ""
            n_ = len([o for o in ops if o.type == A.ARM_OP_REG])
            if nm in ("ldrd", "strd"):
                r1, r2 = regs[0], regs[1]
                if nm == "ldrd":
                    return f"{pre_} t2=M32(t1); t3=M32(t1+4); {upd} {self.wreg(i, r1.reg, 't2')} {self.wreg(i, r2.reg, 't3')}"
                return f"{pre_} W32(t1,{self.rget(i, r1.reg)}); W32(t1+4,{self.rget(i, r2.reg)}); {upd}"
            rt = regs[0]
            if nm in LD:
                mm, sx = LD[nm]
                val = f"(uint32_t)(int32_t)({sx}){mm}(t1)" if sx else f"(uint32_t){mm}(t1)"
                if self.rn(i, rt.reg) == "pc":
                    return f"{pre_} t2={val}; {upd} {self.jump(i, 't2')}"
                return f"{pre_} t2={val}; {upd} {self.wreg(i, rt.reg, 't2')}"
            src = self.rget(i, rt.reg)
            return f"{pre_} {ST[nm]}(t1,{src}); {upd}"
        if nm in ("ldrex", "ldrexb", "ldrexh", "ldrexd"):
            mo = ops[-1]
            a = self.maddr(i, mo)
            if nm == "ldrexd":
                return f"t1={a}; q0=M64(t1); rt_excl_set(t1,q0); {self.wreg(i, ops[0].reg, '(uint32_t)q0')} {self.wreg(i, ops[1].reg, '(uint32_t)(q0>>32)')}"
            mm = {"ldrex": "M32", "ldrexb": "M8", "ldrexh": "M16"}[nm]
            return f"t1={a}; t2={mm}(t1); rt_excl_set(t1,t2); {self.wreg(i, ops[0].reg, 't2')}"
        if nm in ("strex", "strexb", "strexh", "strexd"):
            mo = ops[-1]
            a = self.maddr(i, mo)
            sz = {"strex": 32, "strexb": 8, "strexh": 16, "strexd": 64}[nm]
            if nm == "strexd":
                val = f"(((uint64_t){self.rget(i, ops[2].reg)}<<32)|{self.rget(i, ops[1].reg)})"
            else:
                val = self.rget(i, ops[1].reg)
            return f"t1={a}; {self.wreg(i, ops[0].reg, f'rt_excl_store{sz}(t1,{val})')}"
        if nm in ("ldm", "ldmdb", "ldmib", "ldmda", "stm", "stmdb", "stmib", "stmda", "push", "pop"):
            if nm in ("push", "pop"):
                base, regs, wb = "sp", [self.rn(i, o.reg) for o in ops], True
                mode = "db" if nm == "push" else "ia"
                load = nm == "pop"
            else:
                base = self.rget(i, ops[0].reg)
                regs = [self.rn(i, o.reg) for o in ops[1:]]
                wb = i.writeback
                mode = nm[3:] or "ia"
                load = nm.startswith("ldm")
            k = len(regs)
            start = {"ia": "0", "ib": "4", "db": f"-{4 * k}", "da": f"-{4 * k - 4}"}[mode]
            s = f"t1={base}{('+' + start) if not start.startswith('-') and start != '0' else (start if start != '0' else '')};"
            body = []
            pc_load = False
            for n_, r_ in enumerate(regs):
                if load:
                    if r_ == "pc":
                        pc_load = True
                        body.append(f"t2=M32(t1+{4 * n_});")
                    else:
                        if r_ not in CORE: raise Unsupported(f"ldm reg {r_}")
                        body.append(f"{r_}=M32(t1+{4 * n_});")
                else:
                    v_ = r_ if r_ in CORE else None
                    if r_ == "pc": v_ = f"(B+0x{self.pcv(i):X}u)"
                    if v_ is None: raise Unsupported(f"stm reg {r_}")
                    body.append(f"W32(t1+{4 * n_},{v_});")
            s += " " + " ".join(body)
            if wb and not (load and base in regs):
                s += f" {base}={base}{'+' if mode in ('ia', 'ib') else '-'}{4 * k}u;"
            if pc_load:
                s += " " + self.jump(i, "t2")
            return s
        if nm in ("vldr", "vstr"):
            r_ = vreg(i.reg_name(ops[0].reg))
            a = self.maddr(i, ops[1])
            if not r_: raise Unsupported("vldr reg")
            kind, num = r_
            if nm == "vldr":
                return f"V.w[{num}]=M32({a});" if kind == "s" else f"V.x[{num}]=M64({a});"
            return f"W32({a},V.w[{num}]);" if kind == "s" else f"W64({a},V.x[{num}]);"
        if nm in ("vpush", "vpop", "vldmia", "vldmdb", "vstmia", "vstmdb", "vldm", "vstm"):
            if nm in ("vpush", "vpop"):
                base, rl, wb = "sp", ops, True
                mode = "db" if nm == "vpush" else "ia"
                load = nm == "vpop"
            else:
                base = self.rget(i, ops[0].reg)
                rl, wb = ops[1:], i.writeback
                mode = nm[-2:] if nm[-2:] in ("ia", "db") else "ia"
                load = nm.startswith("vld")
            vr = [vreg(i.reg_name(o.reg)) for o in rl]
            if not all(vr): raise Unsupported("vldm regs")
            size = sum(8 if k == "d" else 4 for k, _ in vr)
            s = f"t1={base}{'-' + str(size) if mode == 'db' else ''};"
            off = 0
            for k, num in vr:
                if k == "d":
                    s += f" V.x[{num}]=M64(t1+{off});" if load else f" W64(t1+{off},V.x[{num}]);"
                    off += 8
                else:
                    s += f" V.w[{num}]=M32(t1+{off});" if load else f" W32(t1+{off},V.w[{num}]);"
                    off += 4
            if wb:
                s += f" {base}={base}{'+' if mode == 'ia' else '-'}{size}u;"
            return s
        if nm in ("vld1", "vst1"):
            return self.vld1(i, nm, ops)
        if nm in ("vld2", "vld3", "vld4", "vst2", "vst3", "vst4"):
            return self.vldn(i, nm, ops)
        return None

    def vldn(self, i, nm, ops):
        """VLDn/VSTn (n=2..4), whole registers: memory holds n-element structures, member m of every
        structure lives in the m-th register group (one or two d registers per member)."""
        if any(getattr(o, "vector_index", -1) != -1 for o in ops if o.type == A.ARM_OP_REG):
            raise Unsupported(f"{nm} lane")
        nst = int(nm[3])
        dt = i.mnemonic.split(".")[1] if "." in i.mnemonic else "8"
        esz = int(dt.lstrip("iusf") or 8)
        regs = [vreg(i.reg_name(o.reg)) for o in ops if o.type == A.ARM_OP_REG and vreg(i.reg_name(o.reg))]
        ds = []
        for k, num in regs:
            ds += [2 * num, 2 * num + 1] if k == "q" else [num]
        if len(ds) % nst:
            raise Unsupported(f"{nm} regs")
        per = len(ds) // nst                      # d registers per member
        lanes_per_d = 64 // esz
        count = per * lanes_per_d                 # structures
        arr = {8: "b", 16: "h", 32: "w"}[esz]
        mm = {8: "M8", 16: "M16", 32: "M32"}[esz]
        ww = {8: "W8", 16: "W16", 32: "W32"}[esz]
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        b = self.rget(i, mo.mem.base)
        out = [f"t1={b};"]
        for st in range(count):
            for m in range(nst):
                d = ds[m * per + st // lanes_per_d]
                el = f"V.{arr}[{d * lanes_per_d + st % lanes_per_d}]"
                addr = f"t1+{(st * nst + m) * (esz // 8)}"
                out.append(f"{el}={mm}({addr});" if nm.startswith("vld") else f"{ww}({addr},{el});")
        size = count * nst * (esz // 8)
        extra = ops[-1] if ops[-1].type == A.ARM_OP_REG and not vreg(i.reg_name(ops[-1].reg)) else None
        if extra is not None:
            out.append(f"{b}+={self.rget(i, extra.reg)};")
        elif i.writeback:
            out.append(f"{b}+={size}u;")
        return " ".join(out)

    def vld1(self, i, nm, ops):
        regs = [vreg(i.reg_name(o.reg)) for o in ops if o.type == A.ARM_OP_REG and vreg(i.reg_name(o.reg))]
        mo = next(o for o in ops if o.type == A.ARM_OP_MEM)
        if any(getattr(o, "vector_index", -1) != -1 for o in ops if o.type == A.ARM_OP_REG):
            # single lane: vld1.32 {d0[1]}, [r0]
            lane_op = next(o for o in ops if o.type == A.ARM_OP_REG and getattr(o, "vector_index", -1) != -1)
            k, num = vreg(i.reg_name(lane_op.reg))
            esz = int(i.mnemonic.split(".")[1]) if "." in i.mnemonic else 32
            idx = lane_op.vector_index
            arr = {8: ("b", 8), 16: ("h", 4), 32: ("w", 2)}[esz]
            el = f"V.{arr[0]}[{num * arr[1] + idx}]"
            mm = {8: "M8", 16: "M16", 32: "M32"}[esz]
            ww = {8: "W8", 16: "W16", 32: "W32"}[esz]
            s = f"t1={self.rget(i, mo.mem.base)};"
            s += f" {el}={mm}(t1);" if nm == "vld1" else f" {ww}(t1,{el});"
            size = esz // 8
        else:
            ds = []
            for k, num in regs:
                ds += [2 * num, 2 * num + 1] if k == "q" else [num]
            s = f"t1={self.rget(i, mo.mem.base)};"
            for n_, d in enumerate(ds):
                s += f" V.x[{d}]=M64(t1+{8 * n_});" if nm == "vld1" else f" W64(t1+{8 * n_},V.x[{d}]);"
            size = 8 * len(ds)
        b = self.rget(i, mo.mem.base)
        extra = ops[-1] if ops[-1].type == A.ARM_OP_REG and ops[-1] is not mo and not vreg(i.reg_name(ops[-1].reg)) else None
        if extra is not None:
            s += f" {b}+={self.rget(i, extra.reg)};"
        elif i.writeback:
            s += f" {b}+={size}u;"
        return s

    # ------------------------------------------------------------------ VFP / NEON
    def vfp(self, i, nm, ops):
        mn = i.mnemonic.split(".")
        dts = mn[1:]
        regs = []
        for o in ops:
            if o.type == A.ARM_OP_REG:
                vn = vreg(i.reg_name(o.reg))
                regs.append(vn if vn else ("r", o.reg))
            else:
                regs.append(None)

        def S(k):        # value expression of operand k (scalar)
            kind, num = regs[k]
            if kind == "s": return f"V.s[{num}]"
            if kind == "d": return f"V.d[{num}]"
            raise Unsupported("vector operand")

        def core(k):
            return self.rget(i, ops[k].reg)

        if nm == "vmrs":
            if "apsr_nzcv" in i.op_str:
                return "n=fn; z=fz; c=fc; v=fv;"
            return self.wreg(i, ops[0].reg, "((fn<<31)|(fz<<30)|(fc<<29)|(fv<<28))")
        if nm == "vmsr":
            return ";"
        if nm == "vmov":
            kinds = [r[0] if r else None for r in regs]
            if len(ops) == 2 and ops[1].type == A.ARM_OP_FP:
                kind, num = regs[0]
                if kind == "s": return f"V.s[{num}]={repr(float(ops[1].fp))}f;" if ops[1].fp == ops[1].fp else f"V.s[{num}]=NAN;"
                if kind == "d" and (not dts or dts[0] == "f64"): return f"V.d[{num}]={repr(float(ops[1].fp))};"
                if kind in ("d", "q"):
                    lanes = 2 if kind == "d" else 4
                    base = 2 * num if kind == "d" else 4 * num
                    return " ".join(f"V.s[{base + k}]={repr(float(ops[1].fp))}f;" for k in range(lanes))
            if len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
                kind, num = regs[0]
                dt = dts[0] if dts else "i32"
                imm = ops[1].imm & 0xFFFFFFFFFFFFFFFF
                nd = 1 if kind == "d" else 2
                d0 = num if kind == "d" else 2 * num
                if dt in ("i32", "f32"):
                    w = imm & 0xFFFFFFFF
                    pat = (w << 32) | w
                elif dt == "i16":
                    h = imm & 0xFFFF
                    pat = h * 0x0001000100010001
                elif dt == "i8":
                    pat = (imm & 0xFF) * 0x0101010101010101
                elif dt == "i64":
                    pat = imm
                else:
                    raise Unsupported(f"vmov imm {dt}")
                return " ".join(f"V.x[{d0 + k}]=0x{pat:X}ull;" for k in range(nd))
            if len(ops) == 2 and kinds[0] in ("s", "d", "q") and kinds[1] == kinds[0] and \
                    getattr(ops[0], "vector_index", -1) == -1 and getattr(ops[1], "vector_index", -1) == -1:
                a, b = regs[0], regs[1]
                if a[0] == "s": return f"V.w[{a[1]}]=V.w[{b[1]}];"
                if a[0] == "d": return f"V.x[{a[1]}]=V.x[{b[1]}];"
                return f"V.x[{2 * a[1]}]=V.x[{2 * b[1]}]; V.x[{2 * a[1] + 1}]=V.x[{2 * b[1] + 1}];"
            if len(ops) == 2 and kinds[0] == "r" and kinds[1] == "s":
                return self.wreg(i, ops[0].reg, f"V.w[{regs[1][1]}]")
            if len(ops) == 2 and kinds[0] == "s" and kinds[1] == "r":
                return f"V.w[{regs[0][1]}]={core(1)};"
            if len(ops) == 3 and kinds == ["r", "r", "d"]:
                d = regs[2][1]
                return f"t2=V.w[{2 * d}]; t3=V.w[{2 * d + 1}]; {self.wreg(i, ops[0].reg, 't2')} {self.wreg(i, ops[1].reg, 't3')}"
            if len(ops) == 3 and kinds == ["d", "r", "r"]:
                d = regs[0][1]
                return f"V.w[{2 * d}]={core(1)}; V.w[{2 * d + 1}]={core(2)};"
            if len(ops) == 4 and kinds == ["r", "r", "s", "s"]:
                return f"t2=V.w[{regs[2][1]}]; t3=V.w[{regs[3][1]}]; {self.wreg(i, ops[0].reg, 't2')} {self.wreg(i, ops[1].reg, 't3')}"
            if len(ops) == 4 and kinds == ["s", "s", "r", "r"]:
                return f"V.w[{regs[0][1]}]={core(2)}; V.w[{regs[1][1]}]={core(3)};"
            if len(ops) == 2 and getattr(ops[0], "vector_index", -1) != -1 and kinds[1] == "r":
                esz = int(dts[0]) if dts and dts[0].isdigit() else 32
                d, idx = regs[0][1], ops[0].vector_index
                arr = {8: ("b", 8), 16: ("h", 4), 32: ("w", 2)}[esz]
                return f"V.{arr[0]}[{d * arr[1] + idx}]={core(1)};"
            if len(ops) == 2 and kinds[0] == "r" and getattr(ops[1], "vector_index", -1) != -1:
                dt = dts[0] if dts else "32"
                esz = int(dt[1:]) if dt[0] in "su" else int(dt)
                d, idx = regs[1][1], ops[1].vector_index
                arr = {8: ("b", 8), 16: ("h", 4), 32: ("w", 2)}[esz]
                el = f"V.{arr[0]}[{d * arr[1] + idx}]"
                if dt.startswith("s") and esz < 32:
                    el = f"(uint32_t)(int32_t)(int{esz}_t){el}"
                return self.wreg(i, ops[0].reg, el)
            raise Unsupported(f"vmov {i.op_str}")

        scalar = all(r is None or r[0] in ("s", "d", "r") for r in regs) and not any(r and r[0] == "q" for r in regs)
        is_f64 = any(r and r[0] == "d" for r in regs) and (not dts or dts[0] == "f64")
        NEON_ONLY = ("vorr", "vand", "veor", "vbic", "vorn", "vbsl", "vbit", "vbif", "vmvn", "vtbl", "vtbx", "vpadd",
                     "vceq", "vcgt", "vcge", "vclt", "vcle", "vtst", "vshl", "vext", "vdup", "vmovl", "vmovn", "vzip", "vuzp", "vtrn", "vswp", "vrev64", "vrev32")
        vec = nm in NEON_ONLY or any(r and r[0] == "q" for r in regs) or (any(r and r[0] == "d" for r in regs) and dts and dts[0] != "f64"
                                                     and nm not in ("vcvt", "vcvtr"))
        if vec:
            return self.neon(i, nm, dts, regs, ops)

        if nm in ("vadd", "vsub", "vmul", "vdiv", "vnmul"):
            op = {"vadd": "+", "vsub": "-", "vmul": "*", "vdiv": "/", "vnmul": "*"}[nm]
            if len(ops) == 2:
                d, a, b = S(0), S(0), S(1)
            else:
                d, a, b = S(0), S(1), S(2)
            e = f"({a}{op}{b})"
            if nm == "vnmul": e = f"(-{e})"
            return f"{d}={e};"
        if nm in ("vmla", "vmls", "vnmla", "vnmls", "vfma", "vfms", "vfnma", "vfnms"):
            d, a, b = S(0), S(1), S(2)
            f_ = "fmaf" if regs[0][0] == "s" else "fma"
            e = {"vmla": f"{d}+{a}*{b}", "vmls": f"{d}-{a}*{b}", "vnmla": f"-{d}-{a}*{b}", "vnmls": f"-{d}+{a}*{b}",
                 "vfma": f"{f_}({a},{b},{d})", "vfms": f"{f_}(-{a},{b},{d})", "vfnma": f"{f_}(-{a},{b},-{d})",
                 "vfnms": f"{f_}({a},{b},-{d})"}[nm]
            return f"{d}={e};"
        if nm in ("vneg", "vabs", "vsqrt"):
            d, a = S(0), S(1)
            f32 = regs[0][0] == "s"
            e = {"vneg": f"-{a}", "vabs": f"{'fabsf' if f32 else 'fabs'}({a})", "vsqrt": f"{'sqrtf' if f32 else 'sqrt'}({a})"}[nm]
            return f"{d}={e};"
        if nm in ("vcmp", "vcmpe"):
            a = S(0)
            b = S(1) if len(ops) > 1 and regs[1] else "0.0"
            return (f"{{ double a_={a}, b_={b}; if(a_!=a_||b_!=b_){{fn=0;fz=0;fc=1;fv=1;}} else if(a_==b_){{fn=0;fz=1;fc=1;fv=0;}}"
                    f" else if(a_<b_){{fn=1;fz=0;fc=0;fv=0;}} else {{fn=0;fz=0;fc=1;fv=0;}} }}")
        if nm in ("vcvt", "vcvtr", "vcvtb", "vcvtt"):
            if nm in ("vcvtb", "vcvtt"):
                raise Unsupported(nm)
            to, frm = dts[0], dts[1]
            if len(ops) == 3:          # fixed point
                fb = ops[2].imm
                if to in ("f32", "f64") and frm in ("s32", "u32"):
                    ct = "int32_t" if frm == "s32" else "uint32_t"
                    d = S(0)
                    return f"{d}=(({ct})V.w[{regs[1][1] * (1 if regs[1][0] == 's' else 2)}])/{float(1 << fb)};"
                raise Unsupported(f"vcvt fixed {to}.{frm}")
            src, dst = regs[1], regs[0]
            if to in ("f64", "f32") and frm in ("f32", "f64"):
                return f"{S(0)}={S(1)};"
            if to in ("s32", "u32"):
                fnc = "cvt_s32" if to == "s32" else "cvt_u32"
                mode = "1" if nm == "vcvtr" else "0"
                return f"V.w[{dst[1]}]={fnc}((double){S(1)},{mode});"
            if to in ("f32", "f64") and frm in ("s32", "u32"):
                ct = "(int32_t)" if frm == "s32" else ""
                return f"{S(0)}={ct}V.w[{src[1]}];"
            raise Unsupported(f"vcvt {to}.{frm}")
        if nm in ("vmax", "vmin", "vmaxnm", "vminnm"):
            d, a, b = S(0), S(1), S(2)
            f_ = ("fmaxf" if "max" in nm else "fminf") if regs[0][0] == "s" else ("fmax" if "max" in nm else "fmin")
            return f"{d}={f_}({a},{b});"
        if nm in ("vsel", "vrint", "vrintz", "vrintm", "vrintp", "vrinta", "vrintn", "vrintx", "vrintr"):
            raise Unsupported(nm)
        return None

    def neon(self, i, nm, dts, regs, ops):
        """Element-wise NEON on d (64-bit) / q (128-bit) registers."""
        dt = dts[0] if dts else "i32"
        if nm in ("vmovl", "vmovn"):
            src_sz = int(dt[1:])
            sgn_ = dt[0] == "s"
            (dk, dn), (sk, sn) = regs[0], regs[1]
            A_ = {8: "b", 16: "h", 32: "w", 64: "x"}
            T_ = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t"}
            if nm == "vmovl":          # d (n lanes of src_sz) -> q (n lanes of 2*src_sz)
                cnt = 64 // src_sz
                sa, da = A_[src_sz], A_[2 * src_sz]
                s0, d0 = sn * cnt, dn * 2 * cnt // 2 * 1
                d0 = dn * (128 // (2 * src_sz))
                cast = f"({T_[2 * src_sz]})({T_[src_sz]})" if sgn_ else ""
                body = " ".join(f"T[{k}]={cast}V.{sa}[{s0 + k}];" for k in range(cnt))
                store = " ".join(f"V.{da}[{d0 + k}]=T[{k}];" for k in range(cnt))
                return f"{{ uint64_t T[{cnt}]; {body} {store} }}"
            cnt = 64 // (src_sz // 2)      # vmovn: q (cnt lanes of src_sz) -> d (cnt lanes of src_sz/2)
            sa, da = A_[src_sz], A_[src_sz // 2]
            s0 = sn * (128 // src_sz)
            d0 = dn * cnt
            body = " ".join(f"T[{k}]=V.{sa}[{s0 + k}];" for k in range(cnt))
            store = " ".join(f"V.{da}[{d0 + k}]=T[{k}];" for k in range(cnt))
            return f"{{ uint64_t T[{cnt}]; {body} {store} }}"
        if dt[0] in "iusf" and dt[1:].isdigit():
            esz = int(dt[1:])
        elif dt.isdigit():
            esz = int(dt)
        else:
            esz = 32

        def lanes(r):
            kind, num = r
            nbytes = 8 if kind == "d" else 16
            first = num * 8 if kind == "d" else num * 16
            return first, nbytes // (esz // 8)

        arr = {8: "b", 16: "h", 32: "w", 64: "x"}[esz]
        farr = "s" if esz == 32 else "d"
        sty = {8: "int8_t", 16: "int16_t", 32: "int32_t", 64: "int64_t"}[esz]
        uty = {8: "uint8_t", 16: "uint16_t", 32: "uint32_t", 64: "uint64_t"}[esz]
        isf = dt.startswith("f")
        sgn = dt.startswith("s")

        def el(r, k, f=False):
            first, cnt = lanes(r)
            idx = first // (esz // 8) + k
            return f"V.{farr if f else arr}[{idx}]"

        def loop(n, body_fn):
            # compute into temps first (operands may alias the destination)
            tmps = " ".join(f"T[{k}]=({body_fn(k)});" for k in range(n))
            return tmps

        r0 = regs[0]
        if r0 is None or r0[0] not in ("d", "q"):
            raise Unsupported(f"neon {nm} {i.op_str}")
        _, cnt = lanes(r0)
        tty = "double" if (isf and esz == 64) else ("float" if isf else uty)

        def emit(expr_k, f=isf):
            body = " ".join(f"T[{k}]=({expr_k(k)});" for k in range(cnt))
            store = " ".join(f"{el(r0, k, f)}=T[{k}];" for k in range(cnt))
            return f"{{ {tty} T[{cnt}]; {body} {store} }}"

        three = len(ops) == 3 and regs[1] and regs[2]
        a_ = regs[1] if three else (regs[0] if len(ops) == 2 else None)
        b_ = regs[2] if three else (regs[1] if len(ops) == 2 else None)

        if nm in ("vadd", "vsub", "vmul") and three or (nm in ("vadd", "vsub", "vmul") and len(ops) == 2):
            op = {"vadd": "+", "vsub": "-", "vmul": "*"}[nm]
            if regs[-1] and getattr(ops[-1], "vector_index", -1) != -1:   # by scalar
                si = ops[-1].vector_index
                sk, sn = regs[-1]
                sc = f"V.{farr if isf else arr}[{sn * (64 // esz) + si}]"
                return emit(lambda k: f"{el(a_, k, isf)}{op}{sc}")
            return emit(lambda k: f"{el(a_, k, isf)}{op}{el(b_, k, isf)}")
        if nm in ("vmla", "vmls"):
            op = "+" if nm == "vmla" else "-"
            if getattr(ops[-1], "vector_index", -1) != -1:
                si = ops[-1].vector_index
                sk, sn = regs[-1]
                sc = f"V.{farr if isf else arr}[{sn * (64 // esz) + si}]"
                return emit(lambda k: f"{el(r0, k, isf)}{op}{el(regs[1], k, isf)}*{sc}")
            return emit(lambda k: f"{el(r0, k, isf)}{op}{el(regs[1], k, isf)}*{el(regs[2], k, isf)}")
        if nm in ("vand", "vorr", "veor", "vbic", "vorn"):
            if len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
                raise Unsupported(f"{nm} imm")
            first0, _ = r0[1], 0
            nd = 1 if r0[0] == "d" else 2
            d0 = r0[1] if r0[0] == "d" else 2 * r0[1]
            a0 = a_[1] if a_[0] == "d" else 2 * a_[1]
            b0 = b_[1] if b_[0] == "d" else 2 * b_[1]
            op = {"vand": "&", "vorr": "|", "veor": "^", "vbic": "&~", "vorn": "|~"}[nm]
            return " ".join(f"V.x[{d0 + k}]=V.x[{a0 + k}]{op}V.x[{b0 + k}];" for k in range(nd))
        if nm in ("vbsl", "vbit", "vbif"):
            nd = 1 if r0[0] == "d" else 2
            d0 = r0[1] if r0[0] == "d" else 2 * r0[1]
            a0 = regs[1][1] if regs[1][0] == "d" else 2 * regs[1][1]
            b0 = regs[2][1] if regs[2][0] == "d" else 2 * regs[2][1]
            out = []
            for k in range(nd):
                d, a, b = f"V.x[{d0 + k}]", f"V.x[{a0 + k}]", f"V.x[{b0 + k}]"
                if nm == "vbsl": out.append(f"{d}=({d}&{a})|(~{d}&{b});")
                elif nm == "vbit": out.append(f"{d}=({d}&~{b})|({a}&{b});")
                else: out.append(f"{d}=({d}&{b})|({a}&~{b});")
            return " ".join(out)
        if nm == "vdup":
            if regs[1] and regs[1][0] == "r":
                x = self.rget(i, ops[1].reg)
                return emit(lambda k: f"({uty}){x}", False)
            si = ops[1].vector_index
            sk, sn = regs[1]
            sc = f"V.{arr}[{sn * (64 // esz) + si}]"
            return emit(lambda k: sc, False)
        if nm in ("vneg", "vabs"):
            if isf:
                fa = "fabsf" if esz == 32 else "fabs"
                return emit(lambda k: (f"-{el(regs[1], k, True)}" if nm == "vneg" else f"{fa}({el(regs[1], k, True)})"))
            return emit(lambda k: (f"({uty})(-({sty}){el(regs[1], k)})" if nm == "vneg"
                                   else f"({uty})((({sty}){el(regs[1], k)})<0?-({sty}){el(regs[1], k)}:({sty}){el(regs[1], k)})"), False)
        if nm in ("vmax", "vmin"):
            if isf:
                f_ = "fmaxf" if nm == "vmax" else "fminf"
                return emit(lambda k: f"{f_}({el(a_, k, True)},{el(b_, k, True)})")
            ty = sty if sgn else uty
            cmp = ">" if nm == "vmax" else "<"
            return emit(lambda k: f"(({ty}){el(a_, k)}{cmp}({ty}){el(b_, k)})?{el(a_, k)}:{el(b_, k)}", False)
        if nm == "vcvt" and len(dts) == 2:
            to, frm = dts
            esz_ = 32
            cnt_ = 2 if r0[0] == "d" else 4
            f0 = (r0[1] * 2) if r0[0] == "d" else r0[1] * 4
            s0 = (regs[1][1] * 2) if regs[1][0] == "d" else regs[1][1] * 4
            if len(ops) == 3:
                fb = ops[2].imm
                if to == "f32" and frm in ("s32", "u32"):
                    ct = "int32_t" if frm == "s32" else "uint32_t"
                    return " ".join(f"V.s[{f0 + k}]=(({ct})V.w[{s0 + k}])/{float(1 << fb)}f;" for k in range(cnt_))
                if to in ("s32", "u32") and frm == "f32":
                    fnc = "cvt_s32" if to == "s32" else "cvt_u32"
                    return " ".join(f"V.w[{f0 + k}]={fnc}((double)V.s[{s0 + k}]*{float(1 << fb)},0);" for k in range(cnt_))
                raise Unsupported("vcvt vec fixed")
            if to == "f32" and frm in ("s32", "u32"):
                ct = "(int32_t)" if frm == "s32" else ""
                return f"{{ float T[{cnt_}]; " + " ".join(f"T[{k}]={ct}V.w[{s0 + k}];" for k in range(cnt_)) + " " + \
                    " ".join(f"V.s[{f0 + k}]=T[{k}];" for k in range(cnt_)) + " }"
            if to in ("s32", "u32") and frm == "f32":
                fnc = "cvt_s32" if to == "s32" else "cvt_u32"
                return " ".join(f"V.w[{f0 + k}]={fnc}((double)V.s[{s0 + k}],0);" for k in range(cnt_))
            raise Unsupported(f"vcvt vec {to}.{frm}")
        if nm in ("vshl", "vshr", "vsra") and len(ops) == 3 and ops[2].type == A.ARM_OP_IMM:
            sh = ops[2].imm
            if nm == "vshl":
                return emit(lambda k: f"({uty})({el(regs[1], k)}<<{sh})", False)
            ty = sty if sgn else uty
            if nm == "vshr":
                return emit(lambda k: f"({uty})(({ty}){el(regs[1], k)}>>{sh})", False)
            return emit(lambda k: f"({uty})({el(r0, k)}+(({ty}){el(regs[1], k)}>>{sh}))", False)
        if nm == "vext" and len(ops) == 4:
            imm = ops[3].imm
            nb = 8 if r0[0] == "d" else 16
            d0 = r0[1] * nb
            a0 = regs[1][1] * nb
            b0 = regs[2][1] * nb
            return (f"{{ uint8_t c_[{2 * nb}]; memcpy(c_,&V.b[{a0}],{nb}); memcpy(c_+{nb},&V.b[{b0}],{nb}); "
                    f"memcpy(&V.b[{d0}],c_+{imm * (esz // 8) if dt.isdigit() or dt[1:].isdigit() else imm},{nb}); }}")
        nd0 = 1 if r0[0] == "d" else 2
        x0 = r0[1] if r0[0] == "d" else 2 * r0[1]
        def xs(r): return r[1] if r[0] == "d" else 2 * r[1]
        if nm in ("vorr", "vbic", "vand") and len(ops) == 2 and ops[1].type == A.ARM_OP_IMM:
            w = ops[1].imm & 0xFFFFFFFF
            if dt in ("i16",): w = (w & 0xFFFF) * 0x10001
            pat = (w << 32) | w
            op = {"vorr": "|=", "vbic": "&=~", "vand": "&="}[nm]
            return " ".join(f"V.x[{x0 + k}]{op}0x{pat:X}ull;" for k in range(nd0))
        if nm == "vmvn":
            if ops[1].type == A.ARM_OP_IMM:
                w = ops[1].imm & 0xFFFFFFFF
                if dt == "i16": w = (w & 0xFFFF) * 0x10001
                if dt == "i8": w = (w & 0xFF) * 0x01010101
                pat = ~((w << 32) | w) & 0xFFFFFFFFFFFFFFFF
                return " ".join(f"V.x[{x0 + k}]=0x{pat:X}ull;" for k in range(nd0))
            s1 = xs(regs[1])
            return " ".join(f"V.x[{x0 + k}]=~V.x[{s1 + k}];" for k in range(nd0))
        if nm == "vswp":
            s1 = xs(regs[1])
            return " ".join(f"{{ uint64_t t_=V.x[{x0 + k}]; V.x[{x0 + k}]=V.x[{s1 + k}]; V.x[{s1 + k}]=t_; }}" for k in range(nd0))
        if nm in ("vceq", "vcgt", "vcge", "vclt", "vcle", "vtst"):
            a_r = regs[1] if len(ops) >= 3 else r0
            zero = ops[-1].type == A.ARM_OP_IMM
            b_r = None if zero else regs[-1]
            ty = ("float" if esz == 32 else "double") if isf else (sty if (sgn or nm in ("vclt", "vcle") and zero) else uty)
            if dt.startswith("i") and nm != "vceq" and nm != "vtst": ty = sty
            cmp = {"vceq": "==", "vcgt": ">", "vcge": ">=", "vclt": "<", "vcle": "<="}.get(nm)
            def cmpk(k):
                a = f"({ty}){el(a_r, k, isf)}"
                b = "0" if zero else f"({ty}){el(b_r, k, isf)}"
                if nm == "vtst": return f"(({el(a_r, k)}&{el(b_r, k)})?({uty})-1:({uty})0)"
                return f"(({a}{cmp}{b})?({uty})-1:({uty})0)"
            return emit(cmpk, False)
        if nm == "vshl" and len(ops) == 3 and regs[2]:
            ty = sty if sgn else uty
            def shk(k):
                a, n_ = el(regs[1], k), f"((int32_t)(int8_t){el(regs[2], k)})"
                if sgn:
                    right = f"(-{n_}>={esz}?(({sty}){a}<0?({uty})-1:({uty})0):({uty})((({sty}){a})>>(-{n_})))"
                else:
                    right = f"(-{n_}>={esz}?({uty})0:({uty})({a}>>(-{n_})))"
                return f"({n_}>=0?({n_}>={esz}?({uty})0:({uty})({a}<<{n_})):{right})"
            return emit(shk, False)
        if nm == "vpadd":
            a_r, b_r = regs[1], regs[2]
            half = cnt // 2 if r0[0] == "q" else cnt // 2
            _, ca = lanes(a_r)
            def pk(k):
                src, j = (a_r, 2 * k) if k < ca // 2 else (b_r, 2 * (k - ca // 2))
                if isf: return f"{el(src, j, True)}+{el(src, j + 1, True)}"
                return f"({uty})({el(src, j)}+{el(src, j + 1)})"
            return emit(pk)
        if nm in ("vtbl", "vtbx"):
            tab = [vreg(i.reg_name(o.reg)) for o in ops[1:-1] if o.type == A.ARM_OP_REG]
            tds = []
            for k_, n_ in tab:
                tds += [2 * n_, 2 * n_ + 1] if k_ == "q" else [n_]
            idx_r = regs[-1]
            nbytes = 8 * len(tds)
            tbl = "{ uint8_t tb_[%d]; %s " % (nbytes, " ".join(f"memcpy(tb_+{8 * j},&V.b[{8 * d}],8);" for j, d in enumerate(tds)))
            body = " ".join(f"T[{k}]=V.b[{8 * idx_r[1] + k}]<{nbytes}?tb_[V.b[{8 * idx_r[1] + k}]]:{'V.b[%d]' % (8 * r0[1] + k) if nm == 'vtbx' else '0'};" for k in range(8))
            store = " ".join(f"V.b[{8 * r0[1] + k}]=T[{k}];" for k in range(8))
            return tbl + f"uint8_t T[8]; {body} {store} }}"
        if nm in ("vrev64", "vrev32"):
            grp = 64 if nm == "vrev64" else 32
            per = grp // esz
            return emit(lambda k: el(regs[1], (k // per) * per + (per - 1 - k % per)), False)
        raise Unsupported(f"neon {nm}")
