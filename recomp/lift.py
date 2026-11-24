"""x86-32 → C static lifter.

Each guest function becomes `void F_<off>(void)`. Guest GPRs are C locals inside a function and are
synced to globals (g_eax…) around calls/returns. Flags are locals computed per instruction; clang
deletes the ones nobody reads. Guest addresses equal wasm addresses: the image is loaded at runtime
base `B`, so every code/data address that comes from EIP becomes `B + off`.
"""
import iced_x86 as I

R = I.Register
MN = {v: k for k, v in vars(I.Mnemonic).items() if not k.startswith("_")}
GPR32 = {R.EAX: "eax", R.ECX: "ecx", R.EDX: "edx", R.EBX: "ebx",
         R.ESP: "esp", R.EBP: "ebp", R.ESI: "esi", R.EDI: "edi"}
GPR16 = {R.AX: "eax", R.CX: "ecx", R.DX: "edx", R.BX: "ebx",
         R.SP: "esp", R.BP: "ebp", R.SI: "esi", R.DI: "edi"}
GPR8L = {R.AL: "eax", R.CL: "ecx", R.DL: "edx", R.BL: "ebx"}
GPR8H = {R.AH: "eax", R.CH: "ecx", R.DH: "edx", R.BH: "ebx"}
XMM = {getattr(R, f"XMM{i}"): i for i in range(8)}
REGS = ["eax", "ecx", "edx", "ebx", "esp", "ebp", "esi", "edi"]
MASK = {8: "0xFFu", 16: "0xFFFFu", 32: "0xFFFFFFFFu"}
SIGN = {8: 7, 16: 15, 32: 31}

COND = {
    "O": "of", "NO": "!of", "B": "cf", "AE": "!cf", "E": "zf", "NE": "!zf",
    "BE": "(cf|zf)", "A": "!(cf|zf)", "S": "sf", "NS": "!sf", "P": "pf", "NP": "!pf",
    "L": "(sf!=of)", "GE": "(sf==of)", "LE": "(zf|(sf!=of))", "G": "(!zf&&(sf==of))",
}


import os
# Debug aid: TRACE_FUNCS=6ee50,334730 at build time logs every entry to those functions.
TRACE_FUNCS = {int(x, 16) for x in os.environ.get("TRACE_FUNCS", "").split(",") if x}
TRACE_RET = {int(x, 16) for x in os.environ.get("TRACE_RET", "").split(",") if x}   # also report EAX when these return


class Unsupported(Exception):
    pass


import re
_FLAG = {"of": 1, "sf": 2, "zf": 4, "cf": 16, "pf": 32}
_FLAG_STMT = re.compile(r"(?:(?<=^)|(?<=[\s;{]))(cf|zf|sf|of|pf)=(?!=)([^;{}]*);")


def strip_dead_flags(code, live):
    if live == 0x3F:
        return code
    return _FLAG_STMT.sub(lambda mt: mt.group(0) if live & _FLAG[mt.group(1)] else "", code)


class FuncLifter:
    def __init__(self, prog, start, end):
        self.p, self.start, self.end = prog, start, end
        self.out = []
        self.unsupported = []
        self.uses_setjmp = False

    # ---------- operand helpers ----------
    def addr(self, ins):
        parts = []
        if ins.memory_base == R.EIP:
            raise Unsupported("eip-relative")
        if ins.memory_base != R.NONE:
            parts.append(GPR32[ins.memory_base])
        if ins.memory_index != R.NONE:
            s = ins.memory_index_scale
            parts.append(f"{GPR32[ins.memory_index]}*{s}" if s > 1 else GPR32[ins.memory_index])
        disp = ins.memory_displacement & 0xFFFFFFFF
        if disp or not parts:
            parts.append(f"0x{disp:X}u")
        a = "+".join(parts)
        if ins.memory_segment == R.GS:
            a = f"g_gs+{a}"
        elif ins.memory_segment == R.FS:
            a = f"g_fs+{a}"
        return f"(uint32_t)({a})"

    def size(self, ins, i):
        k = ins.op_kind(i)
        if k == I.OpKind.REGISTER:
            return I.RegisterExt.size(ins.op_register(i)) * 8
        if k == I.OpKind.MEMORY:
            return I.MemorySizeExt.size(ins.memory_size) * 8
        if k in (I.OpKind.IMMEDIATE8, I.OpKind.IMMEDIATE8TO32, I.OpKind.IMMEDIATE16, I.OpKind.IMMEDIATE32,
                 I.OpKind.IMMEDIATE8TO16):
            return None
        raise Unsupported(f"opkind {k}")

    def rd(self, ins, i, size=None):
        k = ins.op_kind(i)
        if k == I.OpKind.REGISTER:
            r = ins.op_register(i)
            if r in GPR32: return GPR32[r]
            if r in GPR16: return f"({GPR16[r]}&0xFFFFu)"
            if r in GPR8L: return f"({GPR8L[r]}&0xFFu)"
            if r in GPR8H: return f"(({GPR8H[r]}>>8)&0xFFu)"
            raise Unsupported(f"reg {r}")
        if k == I.OpKind.MEMORY:
            sz = size or self.size(ins, i)
            return f"M{sz}({self.addr(ins)})"
        if k in (I.OpKind.IMMEDIATE8, I.OpKind.IMMEDIATE16, I.OpKind.IMMEDIATE32,
                 I.OpKind.IMMEDIATE8TO32, I.OpKind.IMMEDIATE8TO16):
            v = ins.immediate(i) & 0xFFFFFFFF
            if size: v &= (1 << size) - 1
            return f"0x{v:X}u"
        raise Unsupported(f"rd opkind {k}")

    def wr(self, ins, i, val, size=None):
        k = ins.op_kind(i)
        if k == I.OpKind.REGISTER:
            r = ins.op_register(i)
            if r in GPR32: return f"{GPR32[r]}=(uint32_t)({val});"
            if r in GPR16: n = GPR16[r]; return f"{n}=({n}&0xFFFF0000u)|((uint32_t)({val})&0xFFFFu);"
            if r in GPR8L: n = GPR8L[r]; return f"{n}=({n}&0xFFFFFF00u)|((uint32_t)({val})&0xFFu);"
            if r in GPR8H: n = GPR8H[r]; return f"{n}=({n}&0xFFFF00FFu)|(((uint32_t)({val})&0xFFu)<<8);"
            raise Unsupported(f"wreg {r}")
        if k == I.OpKind.MEMORY:
            sz = size or self.size(ins, i)
            return f"W{sz}({self.addr(ins)},{val});"
        raise Unsupported(f"wr opkind {k}")

    def xmm(self, ins, i):
        r = ins.op_register(i)
        if r not in XMM: raise Unsupported(f"xmm {r}")
        return f"X[{XMM[r]}]"

    def xsrc(self, ins, i):
        """Pointer to a 128-bit source: register or a temp loaded from memory."""
        if ins.op_kind(i) == I.OpKind.REGISTER:
            return f"&{self.xmm(ins, i)}"
        n = I.MemorySizeExt.size(ins.memory_size)
        return f"xload({self.addr(ins)},{n})"

    # ---------- flag helpers ----------
    @staticmethod
    def szf(res, sz):
        return f"zf=(({res})&{MASK[sz]})==0; sf=(({res})>>{SIGN[sz]})&1; pf=PARITY({res});"

    def lift(self):
        p = self.p
        code = p.image.read(self.start, self.end - self.start)
        insns = list(I.Decoder(32, code, ip=self.start))
        addrs = {i.ip for i in insns}
        has_ijmp = any(MN[i.mnemonic] == "JMP" and i.op_kind(0) != I.OpKind.NEAR_BRANCH32 for i in insns)
        targets = set()
        for i in insns:
            m = MN[i.mnemonic]
            if (m.startswith("J") or m.startswith("LOOP")) and i.op_kind(0) == I.OpKind.NEAR_BRANCH32:
                if self.start <= i.near_branch_target < self.end:
                    targets.add(i.near_branch_target)
        self.cases = ""
        self.cases_complete = True
        self.jt_targets = set()
        if has_ijmp:
            jt = self.jump_tables(insns, addrs)
            self.cases_complete = bool(jt)
            jt = jt if jt else addrs
            targets |= jt
            self.jt_targets = jt
            self.cases = " ".join(f"case 0x{a:X}u: goto L_{a:X};" for a in sorted(jt))
        live = self.flag_liveness(insns, addrs, has_ijmp and not self.cases_complete)
        o = self.out
        hit = f" rt_trace_hit(0x{self.start:X}u);" if self.start in TRACE_FUNCS else ""
        o.append(f"void F_{self.start:X}(void){{ RT_TRACE(0x{self.start:X}u);{hit}")
        o.append(" uint32_t " + ",".join(f"{r}=g_{r}" for r in REGS) + ";")
        o.append(" uint32_t cf=0,zf=0,sf=0,of=0,pf=0; uint32_t t0,t1,t2; uint64_t q0; (void)t0;(void)t1;(void)t2;(void)q0;")
        for ins in insns:
            if ins.ip in targets:
                o.append(f"L_{ins.ip:X}:;")
            try:
                o.append(" " + strip_dead_flags(self.insn(ins, has_ijmp, addrs), live[ins.ip]))
            except Unsupported as e:
                self.unsupported.append((ins.ip, MN.get(ins.mnemonic, "?"), str(e)))
                o.append(f" SYNC_OUT; rt_unsupported(0x{ins.ip:X}u); return; /* {ins} */")
        o.append(" SYNC_OUT; rt_fallthrough(0x%Xu);\n}" % self.start)
        return "\n".join(o)

    def jump_tables(self, insns, addrs):
        """GCC i386 PIC switch: mov r,[ebx+idx*4+disp]; add r,ebx; jmp r  (entries are GOT-relative)."""
        got = self.p.got
        found = set()
        self.cases_complete = True
        for i in insns:
            for k in range(i.op_count):
                if i.op_kind(k) == I.OpKind.MEMORY and i.memory_index_scale == 4 and i.memory_index != R.NONE \
                        and i.memory_base != R.NONE:   # base holds the GOT address (ebx, or a reloaded spill)
                    tab = (got + i.memory_displacement) & 0xFFFFFFFF
                    got_t = []
                    for n in range(4096):
                        e = int.from_bytes(self.p.image.read(tab + 4 * n, 4), "little", signed=True)
                        t = (got + e) & 0xFFFFFFFF
                        if t not in addrs: break
                        got_t.append(t)
                    if len(got_t) >= 2:
                        found.update(got_t)
        return found

    def flag_liveness(self, insns, addrs, conservative):
        ALL = 0x3F
        idx = {ins.ip: n for n, ins in enumerate(insns)}
        succ = []
        for n, ins in enumerate(insns):
            m = MN[ins.mnemonic]
            ss = []
            nxt = n + 1 if n + 1 < len(insns) else None
            if m in ("RET", "RETF"):
                pass
            elif m == "JMP":
                if ins.op_kind(0) == I.OpKind.NEAR_BRANCH32 and ins.near_branch_target in idx:
                    ss.append(idx[ins.near_branch_target])
                elif ins.op_kind(0) != I.OpKind.NEAR_BRANCH32:
                    ss += [idx[t] for t in self.jt_targets]
            elif m.startswith("J") or m.startswith("LOOP"):
                if nxt is not None: ss.append(nxt)
                if ins.near_branch_target in idx: ss.append(idx[ins.near_branch_target])
            elif nxt is not None:
                ss.append(nxt)
            succ.append(ss)
        rd = []; wr = []
        for ins in insns:
            m = MN[ins.mnemonic]
            r, w = ins.rflags_read, ins.rflags_written | ins.rflags_cleared | ins.rflags_set | ins.rflags_undefined
            if m == "CALL": r, w = 0, ALL          # flags are dead across calls (SysV i386 ABI)
            if m in ("SHL", "SHR", "SAR", "ROL", "ROR", "SHLD", "SHRD") and ins.op_kind(ins.op_count - 1) == I.OpKind.REGISTER:
                w = 0                              # count may be 0 → flags preserved
            rd.append(r & ALL); wr.append(w & ALL)
        lin = [0] * len(insns); lout = [0] * len(insns)
        changed = True
        while changed:
            changed = False
            for n in range(len(insns) - 1, -1, -1):
                o = 0
                for s_ in succ[n]: o |= lin[s_]
                i_ = rd[n] | (o & ~wr[n])
                if o != lout[n] or i_ != lin[n]:
                    lout[n], lin[n] = o, i_; changed = True
        if conservative:
            return {ins.ip: ALL for ins in insns}
        return {ins.ip: lout[n] | rd[n] for n, ins in enumerate(insns)}

    # ---------- instructions ----------
    def call(self, target):
        return self.p.call_expr(target)

    def insn(self, ins, has_ijmp, addrs):
        m = MN[ins.mnemonic]
        nxt = ins.next_ip
        rd, wr = self.rd, self.wr

        # ---- control flow ----
        if m == "CALL":
            ret = f"esp-=4; W32(esp,B+0x{nxt:X}u); SYNC_OUT;"
            if ins.op_kind(0) == I.OpKind.NEAR_BRANCH32:
                t = ins.near_branch_target
                if t == nxt:  # call $+5 ; pop reg  (PC materialisation)
                    return f"esp-=4; W32(esp,B+0x{nxt:X}u);"
                if t in self.p.setjmp_plts:
                    # setjmp returns twice: run the host setjmp in *this* C frame.
                    self.uses_setjmp = True
                    return (f"SYNC_OUT; if(setjmp(*rt_setjmp_slot(M32(esp)))){{ SYNC_IN; eax=g_eax; }} "
                            f"else {{ eax=0; }}")
                return f"{ret} {self.call(t)} SYNC_IN;"
            return f"t0={rd(ins,0,32)}; {ret} rt_dispatch(t0); SYNC_IN;"
        if m in ("RET", "RETF"):
            if m == "RETF": raise Unsupported("retf")
            n = 4 + (ins.immediate(0) if ins.op_count else 0)
            rh = f" rt_trace_ret(0x{self.start:X}u);" if self.start in TRACE_RET else ""
            return f"esp+={n}; SYNC_OUT;{rh} return;"
        if m == "JMP":
            if ins.op_kind(0) == I.OpKind.NEAR_BRANCH32:
                t = ins.near_branch_target
                if self.start <= t < self.end and t in addrs:
                    return f"goto L_{t:X};"
                return f"SYNC_OUT; {self.call(t)} return;"  # tail call
            v = rd(ins, 0, 32)
            if has_ijmp:
                return f"t0={v}; switch(t0-B){{ {self.cases} default: SYNC_OUT; rt_dispatch(t0); return; }}"
            return f"t0={v}; SYNC_OUT; rt_dispatch(t0); return;"
        if m.startswith("J"):
            cc = m[1:]
            t = ins.near_branch_target
            if cc == "ECXZ": cond = "ecx==0"
            elif cc in COND: cond = COND[cc]
            else: raise Unsupported(m)
            if self.start <= t < self.end and t in addrs:
                return f"if({cond}) goto L_{t:X};"
            return f"if({cond}){{ SYNC_OUT; {self.call(t)} return; }}"
        if m.startswith("SET"):
            return wr(ins, 0, COND[m[3:]], 8)
        if m.startswith("CMOV"):
            return f"if({COND[m[4:]]}) {wr(ins,0,rd(ins,1,32))}"

        # ---- data movement ----
        if m == "MOV":
            if ins.op_kind(0) == I.OpKind.REGISTER and I.RegisterExt.is_segment_register(ins.op_register(0)):
                raise Unsupported("mov seg")
            sz = self.size(ins, 0)
            return wr(ins, 0, rd(ins, 1, sz), sz)
        if m == "MOVZX":
            return wr(ins, 0, rd(ins, 1))
        if m == "MOVSX":
            s = self.size(ins, 1)
            c = "int8_t" if s == 8 else "int16_t"
            return wr(ins, 0, f"(uint32_t)(int32_t)({c})({rd(ins,1)})")
        if m == "LEA":
            return wr(ins, 0, self.addr(ins).replace("g_gs+", ""))
        if m == "PUSH":
            v = rd(ins, 0, 32)
            return f"t0={v}; esp-=4; W32(esp,t0);"
        if m == "POP":
            return f"t0=M32(esp); esp+=4; {wr(ins,0,'t0',32)}"
        if m == "LEAVE":
            return "esp=ebp; ebp=M32(esp); esp+=4;"
        if m == "XCHG" and ins.op_kind(0) == I.OpKind.MEMORY and self.size(ins, 0) == 32:
            return f"{{ uint32_t o_=__atomic_exchange_n((uint32_t*)P({self.addr(ins)}),{rd(ins,1)},__ATOMIC_SEQ_CST); {wr(ins,1,'o_',32)} }}"
        if m == "XCHG" and ins.op_kind(1) == I.OpKind.MEMORY and self.size(ins, 1) == 32:
            return f"{{ uint32_t o_=__atomic_exchange_n((uint32_t*)P({self.addr(ins)}),{rd(ins,0)},__ATOMIC_SEQ_CST); {wr(ins,0,'o_',32)} }}"
        if ins.has_lock_prefix and m == "XADD" and ins.op_kind(0) == I.OpKind.MEMORY and self.size(ins, 0) == 32:
            return (f"t1={rd(ins,1)}; t0=__atomic_fetch_add((uint32_t*)P({self.addr(ins)}),t1,__ATOMIC_SEQ_CST); t2=t0+t1; "
                    f"{wr(ins,1,'t0',32)} cf=t2<t0; {self.szf('t2',32)} of=(((t0^t2)&(t1^t2))>>31)&1;")
        if ins.has_lock_prefix and m in ("INC", "DEC") and ins.op_kind(0) == I.OpKind.MEMORY and self.size(ins, 0) == 32:
            fn = "__atomic_add_fetch" if m == "INC" else "__atomic_sub_fetch"
            ov = "0x80000000u" if m == "INC" else "0x7FFFFFFFu"
            return f"t2={fn}((uint32_t*)P({self.addr(ins)}),1u,__ATOMIC_SEQ_CST); of=(t2=={ov}); {self.szf('t2',32)}"
        if ins.has_lock_prefix and m not in ("XADD", "INC", "DEC"):
            raise Unsupported(f"lock {m}")
        if m == "XCHG":
            sz = self.size(ins, 0)
            return f"t0={rd(ins,0)}; t1={rd(ins,1)}; {wr(ins,0,'t1',sz)} {wr(ins,1,'t0',sz)}"
        if m == "XADD":
            sz = self.size(ins, 0)
            return f"t0={rd(ins,0)}; t1={rd(ins,1)}; t2=t0+t1; {wr(ins,1,'t0',sz)} {wr(ins,0,'t2',sz)} " \
                   f"cf=(t2&{MASK[sz]})<(t0&{MASK[sz]}); {self.szf('t2',sz)} of=(((t0^t2)&(t1^t2))>>{SIGN[sz]})&1;"
        if m == "CDQ": return "edx=(uint32_t)((int32_t)eax>>31);"
        if m == "CWDE": return "eax=(uint32_t)(int32_t)(int16_t)eax;"
        if m == "BSWAP": return wr(ins, 0, f"__builtin_bswap32({rd(ins,0)})")
        if m in ("NOP", "MFENCE", "WAIT", "LFENCE", "SFENCE", "PAUSE", "PREFETCHT0", "PREFETCHNTA"):
            return ";"
        if m in ("UD2", "INT3", "HLT"):
            return f"SYNC_OUT; rt_trap(0x{ins.ip:X}u); return;"

        # ---- integer ALU ----
        if m in ("ADD", "SUB", "CMP", "ADC", "SBB"):
            sz = self.size(ins, 0)
            a, b = rd(ins, 0), rd(ins, 1, sz)
            mk, sb = MASK[sz], SIGN[sz]
            if m in ("ADD", "ADC"):
                cin = "+cf" if m == "ADC" else ""
                s = f"t0={a}; t1={b}; t2=(t0+t1{cin})&{mk}; " \
                    f"cf={'(t2<t0)||(cf&&t2==t0)' if m=='ADC' else 't2<t0'}; of=(((t0^t2)&(t1^t2))>>{sb})&1; "
            else:
                bin_ = "-cf" if m == "SBB" else ""
                s = f"t0={a}; t1={b}; t2=(t0-t1{bin_})&{mk}; " \
                    f"cf={'(t0<t1)||(cf&&t0==t1)' if m=='SBB' else 't0<t1'}; of=(((t0^t1)&(t0^t2))>>{sb})&1; "
            s += self.szf("t2", sz)
            if m != "CMP": s += " " + wr(ins, 0, "t2", sz)
            return s
        if m in ("AND", "OR", "XOR", "TEST"):
            sz = self.size(ins, 0)
            op = {"AND": "&", "TEST": "&", "OR": "|", "XOR": "^"}[m]
            if m == "XOR" and ins.op_kind(0) == ins.op_kind(1) == I.OpKind.REGISTER and ins.op_register(0) == ins.op_register(1):
                return f"{wr(ins,0,'0',sz)} cf=0; of=0; sf=0; zf=1; pf=1;"
            s = f"t2=({rd(ins,0)}{op}{rd(ins,1,sz)})&{MASK[sz]}; cf=0; of=0; {self.szf('t2',sz)}"
            if m != "TEST": s += " " + wr(ins, 0, "t2", sz)
            return s
        if m in ("INC", "DEC"):
            sz = self.size(ins, 0)
            op = "+" if m == "INC" else "-"
            ov = f"0x{1 << SIGN[sz]:X}u" if m == "INC" else "0"
            return f"t2=({rd(ins,0)}{op}1)&{MASK[sz]}; of=(t2=={ov}); {self.szf('t2',sz)} {wr(ins,0,'t2',sz)}"
        if m == "NEG":
            sz = self.size(ins, 0)
            return f"t0={rd(ins,0)}; t2=(0u-t0)&{MASK[sz]}; cf=t0!=0; of=t2==0x{1 << SIGN[sz]:X}u; {self.szf('t2',sz)} {wr(ins,0,'t2',sz)}"
        if m == "NOT":
            sz = self.size(ins, 0)
            return wr(ins, 0, f"~{rd(ins,0)}", sz)
        if m in ("SHL", "SHR", "SAR", "ROL", "ROR"):
            sz = self.size(ins, 0)
            cnt = rd(ins, 1, 8) if ins.op_count > 1 else "1"
            mk, sb = MASK[sz], SIGN[sz]
            a = rd(ins, 0)
            if m == "SHL":
                body = f"t2=(t0<<t1)&{mk}; cf=(t0>>({sz}-t1))&1; of=((t2>>{sb})&1)^cf; {self.szf('t2',sz)}"
            elif m == "SHR":
                body = f"t2=(t0&{mk})>>t1; cf=(t0>>(t1-1))&1; of=(t0>>{sb})&1; {self.szf('t2',sz)}"
            elif m == "SAR":
                ty = {8: "int8_t", 16: "int16_t", 32: "int32_t"}[sz]
                body = f"t2=(uint32_t)((int32_t)({ty})t0>>t1)&{mk}; cf=((int32_t)({ty})t0>>(t1-1))&1; of=0; {self.szf('t2',sz)}"
            elif m == "ROL":
                body = f"t1%={sz}; t2=t1?((t0<<t1)|((t0&{mk})>>({sz}-t1)))&{mk}:t0; cf=t2&1;"
            else:
                body = f"t1%={sz}; t2=t1?(((t0&{mk})>>t1)|(t0<<({sz}-t1)))&{mk}:t0; cf=(t2>>{sb})&1;"
            return f"t0={a}; t1=({cnt})&31; if(t1){{ {body} {wr(ins,0,'t2',sz)} }}"
        if m in ("SHLD", "SHRD"):
            cnt = rd(ins, 2, 8)
            a, b = rd(ins, 0), rd(ins, 1)
            if m == "SHLD":
                e = "(t0<<t1)|(t2>>(32-t1))"; c = "(t0>>(32-t1))&1"
            else:
                e = "(t0>>t1)|(t2<<(32-t1))"; c = "(t0>>(t1-1))&1"
            return f"t0={a}; t2={b}; t1=({cnt})&31; if(t1){{ cf={c}; t0={e}; {self.szf('t0',32)} {wr(ins,0,'t0',32)} }}"
        if m == "IMUL":
            if ins.op_count == 1:
                sz = self.size(ins, 0)
                if sz != 32: raise Unsupported("imul8/16")
                return f"q0=(uint64_t)((int64_t)(int32_t)eax*(int64_t)(int32_t){rd(ins,0)}); eax=(uint32_t)q0; edx=(uint32_t)(q0>>32); cf=((int64_t)q0!=(int64_t)(int32_t)eax); of=((int64_t)q0!=(int64_t)(int32_t)eax);"
            a = rd(ins, 1) if ins.op_count == 3 else rd(ins, 0)
            b = rd(ins, 2, 32) if ins.op_count == 3 else rd(ins, 1, 32)
            sz = self.size(ins, 0)
            if sz != 32: raise Unsupported("imul16")
            if ins.op_count == 3 and ins.op_kind(2) == I.OpKind.IMMEDIATE8TO32:
                b = f"(uint32_t){ins.immediate(2) & 0xFFFFFFFF}u"
            return f"q0=(uint64_t)((int64_t)(int32_t)({a})*(int64_t)(int32_t)({b})); cf=((int64_t)q0!=(int64_t)(int32_t)q0); of=((int64_t)q0!=(int64_t)(int32_t)q0); {wr(ins,0,'(uint32_t)q0')}"
        if m == "MUL" and self.size(ins, 0) == 8:
            return f"t0=(eax&0xFFu)*{rd(ins,0)}; eax=(eax&0xFFFF0000u)|(t0&0xFFFFu); cf=(t0>>8)!=0; of=cf;"
        if m == "MUL" and self.size(ins, 0) == 16:
            return f"t0=(eax&0xFFFFu)*{rd(ins,0)}; eax=(eax&0xFFFF0000u)|(t0&0xFFFFu); edx=(edx&0xFFFF0000u)|(t0>>16); cf=(t0>>16)!=0; of=cf;"
        if m == "MUL":
            if self.size(ins, 0) != 32: raise Unsupported("mul8/16")
            return f"q0=(uint64_t)eax*(uint64_t){rd(ins,0)}; eax=(uint32_t)q0; edx=(uint32_t)(q0>>32); cf=edx!=0; of=edx!=0;"
        if m in ("DIV", "IDIV"):
            if self.size(ins, 0) != 32: raise Unsupported("div8/16")
            if m == "DIV":
                return f"t0={rd(ins,0)}; if(!t0){{SYNC_OUT; rt_trap(0x{ins.ip:X}u); return;}} q0=((uint64_t)edx<<32)|eax; eax=(uint32_t)(q0/t0); edx=(uint32_t)(q0%t0);"
            return f"t0={rd(ins,0)}; if(!t0){{SYNC_OUT; rt_trap(0x{ins.ip:X}u); return;}} q0=((uint64_t)edx<<32)|eax; eax=(uint32_t)((int64_t)q0/(int32_t)t0); edx=(uint32_t)((int64_t)q0%(int32_t)t0);"
        if m in ("BT", "BTS"):
            b = rd(ins, 1, 32)
            if ins.op_kind(0) == I.OpKind.MEMORY and ins.op_kind(1) == I.OpKind.REGISTER:
                # bit string: the register's high bits select the dword (signed), low 5 the bit
                a = f"(uint32_t)({self.addr(ins)}+(uint32_t)(((int32_t){b}>>5)*4))"
                s_ = f"t1=({b})&31; t0=M32({a}); cf=(t0>>t1)&1;"
                if m == "BTS": s_ += f" W32({a},t0|(1u<<t1));"
                return s_
            s = f"t0={rd(ins,0)}; t1=({b})&31; cf=(t0>>t1)&1;"
            if m == "BTS": s += " " + wr(ins, 0, "t0|(1u<<t1)", 32)
            return s
        if m == "BSR":
            return f"t0={rd(ins,1)}; zf=t0==0; if(t0) {wr(ins,0,'31-__builtin_clz(t0)')}"
        if m == "BSF":
            return f"t0={rd(ins,1)}; zf=t0==0; if(t0) {wr(ins,0,'__builtin_ctz(t0)')}"
        if m in ("STOSD", "STOSB", "MOVSD", "MOVSB", "STOSW", "MOVSW") and (ins.op_count == 0 or ins.op_kind(0) not in (I.OpKind.REGISTER, I.OpKind.MEMORY)):
            n = {"D": 4, "W": 2, "B": 1}[m[-1]]
            if m.startswith("STOS"):
                one = f"W{n*8}(edi,eax); edi+={n};"
            else:
                one = f"W{n*8}(edi,M{n*8}(esi)); edi+={n}; esi+={n};"
            if ins.has_rep_prefix:
                return f"while(ecx){{ {one} ecx--; }}"
            return one
        if m == "SCASB":
            one = "t0=eax&0xFFu; t1=M8(edi); t2=(t0-t1)&0xFFu; edi++; cf=t0<t1; zf=t2==0; sf=(t2>>7)&1; of=(((t0^t1)&(t0^t2))>>7)&1; pf=PARITY(t2);"
            if ins.has_repne_prefix: return f"while(ecx){{ {one} ecx--; if(zf) break; }}"
            if ins.has_repe_prefix: return f"while(ecx){{ {one} ecx--; if(!zf) break; }}"
            return one
        if m == "STC": return "cf=1;"
        if m == "CLC": return "cf=0;"
        if m == "CMC": return "cf^=1;"
        if m == "CLD": return ";"

        # ---- SSE ----
        r = self.sse(ins, m)
        if r is not None:
            return r
        r = self.x87(ins, m)
        if r is not None:
            return r
        raise Unsupported(m)

    def sse(self, ins, m):
        k0 = ins.op_kind(0) if ins.op_count else None
        reg0 = k0 == I.OpKind.REGISTER and I.RegisterExt.is_xmm(ins.op_register(0))
        mem0 = k0 == I.OpKind.MEMORY

        def is_x(i): return ins.op_kind(i) == I.OpKind.REGISTER and I.RegisterExt.is_xmm(ins.op_register(i))

        if m in ("MOVSS", "MOVSD") and ins.op_count == 2:
            ln, ty = ("u[0]", 32) if m == "MOVSS" else ("q[0]", 64)
            if reg0 and is_x(1):
                return f"{self.xmm(ins,0)}.{ln}={self.xmm(ins,1)}.{ln};"
            if reg0:
                return f"{self.xmm(ins,0)}=XZERO; {self.xmm(ins,0)}.{ln}=M{ty}({self.addr(ins)});"
            return f"W{ty}({self.addr(ins)},{self.xmm(ins,1)}.{ln});"
        if m in ("MOVAPS", "MOVUPS", "MOVDQA", "MOVDQU", "MOVAPD", "MOVUPD"):
            if reg0: return f"{self.xmm(ins,0)}=*{self.xsrc(ins,1)};"
            return f"xstore({self.addr(ins)},&{self.xmm(ins,1)},16);"
        if m in ("MOVD", "MOVQ"):
            if m == "MOVD":
                if reg0: return f"{self.xmm(ins,0)}=XZERO; {self.xmm(ins,0)}.u[0]={self.rd(ins,1,32)};"
                return self.wr(ins, 0, f"{self.xmm(ins,1)}.u[0]", 32)
            if reg0:
                src = f"{self.xmm(ins,1)}.q[0]" if is_x(1) else f"M64({self.addr(ins)})"
                return f"q0={src}; {self.xmm(ins,0)}=XZERO; {self.xmm(ins,0)}.q[0]=q0;"
            return f"W64({self.addr(ins)},{self.xmm(ins,1)}.q[0]);"
        if m in ("MOVLPD",):
            if reg0: return f"{self.xmm(ins,0)}.q[0]=M64({self.addr(ins)});"
            return f"W64({self.addr(ins)},{self.xmm(ins,1)}.q[0]);"
        scal = {"ADDSS": ("f", "+"), "SUBSS": ("f", "-"), "MULSS": ("f", "*"), "DIVSS": ("f", "/"),
                "ADDSD": ("d", "+"), "SUBSD": ("d", "-"), "MULSD": ("d", "*"), "DIVSD": ("d", "/")}
        if m in scal:
            l, op = scal[m]
            src = f"{self.xmm(ins,1)}.{l}[0]" if is_x(1) else (f"MF32({self.addr(ins)})" if l == "f" else f"MF64({self.addr(ins)})")
            return f"{self.xmm(ins,0)}.{l}[0]={self.xmm(ins,0)}.{l}[0]{op}{src};"
        if m in ("MAXSS", "MINSS", "MAXSD", "MINSD", "SQRTSS", "SQRTSD"):
            l = "f" if m.endswith("SS") else "d"
            src = f"{self.xmm(ins,1)}.{l}[0]" if is_x(1) else (f"MF32({self.addr(ins)})" if l == "f" else f"MF64({self.addr(ins)})")
            d = f"{self.xmm(ins,0)}.{l}[0]"
            if m.startswith("SQRT"): return f"{d}={'sqrtf' if l=='f' else 'sqrt'}({src});"
            op = ">" if m.startswith("MAX") else "<"
            return f"{{ __typeof__({d}) s_={src}; {d}=({d}{op}s_)?{d}:s_; }}"
        if m in ("UCOMISS", "COMISS", "UCOMISD", "COMISD"):
            l = "f" if m.endswith("SS") else "d"
            src = f"{self.xmm(ins,1)}.{l}[0]" if is_x(1) else (f"MF32({self.addr(ins)})" if l == "f" else f"MF64({self.addr(ins)})")
            return f"{{ double a_={self.xmm(ins,0)}.{l}[0], b_={src}; int u_=(a_!=a_||b_!=b_); zf=u_||a_==b_; cf=u_||a_<b_; pf=u_; of=0; sf=0; }}"
        if m == "CVTSI2SS": return f"{self.xmm(ins,0)}.f[0]=(float)(int32_t){self.rd(ins,1,32)};"
        if m == "CVTSI2SD": return f"{self.xmm(ins,0)}.d[0]=(double)(int32_t){self.rd(ins,1,32)};"
        if m in ("CVTTSS2SI", "CVTTSD2SI"):
            src = f"{self.xmm(ins,1)}.{'f' if 'SS' in m else 'd'}[0]" if is_x(1) else (f"MF32({self.addr(ins)})" if "SS" in m else f"MF64({self.addr(ins)})")
            return self.wr(ins, 0, f"cvtt_i32((double){src})")
        if m == "CVTSS2SD":
            src = f"{self.xmm(ins,1)}.f[0]" if is_x(1) else f"MF32({self.addr(ins)})"
            return f"{self.xmm(ins,0)}.d[0]=(double){src};"
        if m == "CVTSD2SS":
            src = f"{self.xmm(ins,1)}.d[0]" if is_x(1) else f"MF64({self.addr(ins)})"
            return f"{self.xmm(ins,0)}.f[0]=(float){src};"
        bit = {"XORPS": "^", "XORPD": "^", "PXOR": "^", "ANDPS": "&", "ANDPD": "&", "PAND": "&",
               "ORPS": "|", "ORPD": "|", "POR": "|"}
        if m in bit:
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t *s_={s}; {d}.q[0]{bit[m]}=s_->q[0]; {d}.q[1]{bit[m]}=s_->q[1]; }}"
        if m in ("ANDNPS", "PANDN"):
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t *s_={s}; {d}.q[0]=~{d}.q[0]&s_->q[0]; {d}.q[1]=~{d}.q[1]&s_->q[1]; }}"
        vec = {"ADDPS": ("f", 4, "+"), "SUBPS": ("f", 4, "-"), "MULPS": ("f", 4, "*"),
               "PADDD": ("u", 4, "+"), "PADDQ": ("q", 2, "+"), "PSUBQ": ("q", 2, "-"), "PADDW": ("w", 8, "+")}
        if m in vec:
            l, n, op = vec[m]
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t *s_={s}; for(int i_=0;i_<{n};i_++) {d}.{l}[i_]={d}.{l}[i_]{op}s_->{l}[i_]; }}"
        if m in ("PSRLQ", "PSLLQ", "PSRLD", "PSRAD"):
            d = self.xmm(ins, 0)
            c = self.rd(ins, 1) if ins.op_kind(1) != I.OpKind.REGISTER else f"{self.xmm(ins,1)}.u[0]"
            if m == "PSRLQ": return f"t0={c}; for(int i_=0;i_<2;i_++) {d}.q[i_]=t0>63?0:{d}.q[i_]>>t0;"
            if m == "PSLLQ": return f"t0={c}; for(int i_=0;i_<2;i_++) {d}.q[i_]=t0>63?0:{d}.q[i_]<<t0;"
            if m == "PSRLD": return f"t0={c}; for(int i_=0;i_<4;i_++) {d}.u[i_]=t0>31?0:{d}.u[i_]>>t0;"
            return f"t0={c}; if(t0>31)t0=31; for(int i_=0;i_<4;i_++) {d}.i[i_]={d}.i[i_]>>t0;"
        if m == "PMULUDQ":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t *s_={s}; {d}.q[0]=(uint64_t){d}.u[0]*s_->u[0]; {d}.q[1]=(uint64_t){d}.u[2]*s_->u[2]; }}"
        if m in ("PUNPCKLDQ", "UNPCKLPS"):
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}, d_={d}; {d}.u[0]=d_.u[0]; {d}.u[1]=s_.u[0]; {d}.u[2]=d_.u[1]; {d}.u[3]=s_.u[1]; }}"
        if m == "PUNPCKHDQ":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}, d_={d}; {d}.u[0]=d_.u[2]; {d}.u[1]=s_.u[2]; {d}.u[2]=d_.u[3]; {d}.u[3]=s_.u[3]; }}"
        if m == "PUNPCKLQDQ":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}; {d}.q[1]=s_.q[0]; }}"
        if m == "PUNPCKLBW":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}, d_={d}; for(int i_=0;i_<8;i_++){{ {d}.b[2*i_]=d_.b[i_]; {d}.b[2*i_+1]=s_.b[i_]; }} }}"
        if m == "PUNPCKLWD":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}, d_={d}; for(int i_=0;i_<4;i_++){{ {d}.w[2*i_]=d_.w[i_]; {d}.w[2*i_+1]=s_.w[i_]; }} }}"
        if m == "PSHUFD":
            d, s, im = self.xmm(ins, 0), self.xsrc(ins, 1), ins.immediate(2)
            return f"{{ xmm_t s_=*{s}; for(int i_=0;i_<4;i_++) {d}.u[i_]=s_.u[({im}>>(2*i_))&3]; }}"
        if m == "SHUFPS":
            d, s, im = self.xmm(ins, 0), self.xsrc(ins, 1), ins.immediate(2)
            return (f"{{ xmm_t s_=*{s}, d_={d}; {d}.u[0]=d_.u[{im & 3}]; {d}.u[1]=d_.u[{(im >> 2) & 3}]; "
                    f"{d}.u[2]=s_.u[{(im >> 4) & 3}]; {d}.u[3]=s_.u[{(im >> 6) & 3}]; }}")
        if m == "SHUFPD":
            d, s, im = self.xmm(ins, 0), self.xsrc(ins, 1), ins.immediate(2)
            return f"{{ xmm_t s_=*{s}, d_={d}; {d}.q[0]=d_.q[{im & 1}]; {d}.q[1]=s_.q[{(im >> 1) & 1}]; }}"
        if m == "PSHUFB":
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            return f"{{ xmm_t s_=*{s}, d_={d}; for(int i_=0;i_<16;i_++) {d}.b[i_]=(s_.b[i_]&0x80)?0:d_.b[s_.b[i_]&15]; }}"
        if m == "PALIGNR":
            d, s, im = self.xmm(ins, 0), self.xsrc(ins, 1), ins.immediate(2)
            return f"{{ uint8_t c_[32]; memcpy(c_,{s},16); memcpy(c_+16,&{d},16); for(int i_=0;i_<16;i_++) {d}.b[i_]=(i_+{im})<32?c_[i_+{im}]:0; }}"
        if m in ("CMPSS",):
            d, im = self.xmm(ins, 0), ins.immediate(2)
            src = f"{self.xmm(ins,1)}.f[0]" if is_x(1) else f"MF32({self.addr(ins)})"
            return f"{d}.u[0]=cmp_ps({d}.f[0],{src},{im})?0xFFFFFFFFu:0;"
        if m in ("PCMPEQD", "PCMPGTD", "PCMPGTB"):
            d, s = self.xmm(ins, 0), self.xsrc(ins, 1)
            if m == "PCMPGTB":
                return f"{{ xmm_t s_=*{s}; for(int i_=0;i_<16;i_++) {d}.b[i_]=((int8_t){d}.b[i_]>(int8_t)s_.b[i_])?0xFF:0; }}"
            op = "==" if m == "PCMPEQD" else ">"
            return f"{{ xmm_t s_=*{s}; for(int i_=0;i_<4;i_++) {d}.u[i_]=({d}.i[i_]{op}s_.i[i_])?0xFFFFFFFFu:0; }}"
        if m == "PSHUFHW":
            d, s, im = self.xmm(ins, 0), self.xsrc(ins, 1), ins.immediate(2)
            return f"{{ xmm_t s_=*{s}; {d}.q[0]=s_.q[0]; for(int i_=0;i_<4;i_++) {d}.w[4+i_]=s_.w[4+(({im}>>(2*i_))&3)]; }}"
        return None

    def x87(self, ins, m):
        def msrc(i=0):
            n = I.MemorySizeExt.size(ins.memory_size)
            if MN[ins.mnemonic].startswith("FI"):
                return {2: f"(double)(int16_t)M16({self.addr(ins)})", 4: f"(double)(int32_t)M32({self.addr(ins)})",
                        8: f"(double)(int64_t)M64({self.addr(ins)})"}[n]
            return {4: f"(double)MF32({self.addr(ins)})", 8: f"MF64({self.addr(ins)})"}.get(n) or f"f80_load({self.addr(ins)})"

        def st(i): return f"ST({i})"

        if m == "FLD":
            if ins.op_kind(0) == I.OpKind.MEMORY: return f"fpush({msrc()});"
            return f"fpush(ST({ins.op_register(0) - R.ST0}));"
        if m == "FILD": return f"fpush({msrc()});"
        if m == "FLD1": return "fpush(1.0);"
        if m == "FLDZ": return "fpush(0.0);"
        if m in ("FSTP", "FST"):
            pop = " fpop();" if m == "FSTP" else ""
            if ins.op_kind(0) == I.OpKind.MEMORY:
                n = I.MemorySizeExt.size(ins.memory_size)
                if n == 4: return f"WF32({self.addr(ins)},(float)ST(0));{pop}"
                if n == 8: return f"WF64({self.addr(ins)},ST(0));{pop}"
                if n == 10: return f"f80_store({self.addr(ins)},ST(0));{pop}"
                raise Unsupported("fstp?")
            return f"ST({ins.op_register(0) - R.ST0})=ST(0);{pop}"
        if m in ("FISTP", "FIST", "FISTTP"):
            n = I.MemorySizeExt.size(ins.memory_size)
            conv = "trunc" if m == "FISTTP" else "nearbyint"
            pop = "" if m == "FIST" else " fpop();"
            if n == 4: return f"W32({self.addr(ins)},(uint32_t)(int32_t){conv}(ST(0)));{pop}"
            if n == 8: return f"W64({self.addr(ins)},(uint64_t)(int64_t){conv}(ST(0)));{pop}"
            if n == 2: return f"W16({self.addr(ins)},(uint16_t)(int16_t){conv}(ST(0)));{pop}"
        if m == "FXCH":
            i = ins.op_register(0) - R.ST0 if ins.op_count else 1
            return f"{{ double t_=ST(0); ST(0)=ST({i}); ST({i})=t_; }}"
        if m in ("FNSTCW",): return f"W16({self.addr(ins)},0x037Fu);"
        if m in ("FLDCW",): return ";"
        if m in ("FIMUL", "FISUB", "FDIV", "FDIVR", "FMUL", "FADD", "FSUB", "FSUBR") and ins.op_count == 1 and ins.op_kind(0) == I.OpKind.MEMORY:
            op = {"FIMUL": "ST(0)*=s_", "FISUB": "ST(0)-=s_", "FDIV": "ST(0)/=s_", "FDIVR": "ST(0)=s_/ST(0)",
                  "FMUL": "ST(0)*=s_", "FADD": "ST(0)+=s_", "FSUB": "ST(0)-=s_", "FSUBR": "ST(0)=s_-ST(0)"}[m]
            return f"{{ double s_={msrc()}; {op}; }}"
        return None

    def _u(self, what):
        raise Unsupported(what)
