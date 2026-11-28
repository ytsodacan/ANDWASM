"""Print the const-strings and invokes used by one method in a dex file: dexmethod.py <dex> <class descriptor> <method>"""
import struct, sys
d = open(sys.argv[1], 'rb').read(); want_cls, want_m = sys.argv[2], sys.argv[3]
def u32(o): return struct.unpack_from('<I', d, o)[0]
def uleb(o):
    r = s = 0
    while True:
        b = d[o]; o += 1; r |= (b & 0x7f) << s; s += 7
        if b < 0x80: return r, o
str_n, str_o, type_n, type_o, proto_n, proto_o, field_n, field_o, meth_n, meth_o, cls_n, cls_o = struct.unpack_from('<12I', d, 56)
def string(i):
    o = u32(str_o + 4 * i); _, o = uleb(o); e = d.index(b'\0', o); return d[o:e].decode('utf8', 'replace')
def typ(i): return string(u32(type_o + 4 * i))
def meth(i): c, p, n = struct.unpack_from('<HHI', d, meth_o + 8 * i); return typ(c), string(n)
for k in range(cls_n):
    co = cls_o + 32 * k
    if typ(u32(co)) != want_cls: continue
    cdo = u32(co + 24); o = cdo
    sf, o = uleb(o); inf, o = uleb(o); dm, o = uleb(o); vm, o = uleb(o)
    for _ in range(sf + inf): _, o = uleb(o); _, o = uleb(o)
    idx = 0
    for _ in range(dm + vm):
        di, o = uleb(o); _, o = uleb(o); code, o = uleb(o)
        idx = di if _ == 0 and False else idx
        idx += di
        c, name = meth(idx)
        if name == want_m and code:
            insns_n = u32(code + 12); ins = code + 16
            pc = 0
            while pc < insns_n:
                op = d[ins + 2 * pc]
                if op == 0x1a: print('const-string', repr(string(struct.unpack_from('<H', d, ins + 2 * pc + 2)[0]))); pc += 2
                elif op == 0x1b: print('const-string/jumbo', repr(string(u32(ins + 2 * pc + 2)))); pc += 3
                elif op in (0x6e, 0x6f, 0x70, 0x71, 0x72): print('invoke', meth(struct.unpack_from('<H', d, ins + 2 * pc + 2)[0])); pc += 3
                else: pc += 1
        if _ == 0: pass
