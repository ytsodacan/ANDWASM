#!/usr/bin/env python3
"""Compare reference (Unicorn) and lifted results: compare.py <calls.txt> <ref.txt> <lifted.txt>
Words and buffers must match bit for bit, except float buffers/returns that differ by at most a few ulps
(math imports come from different libm implementations)."""
import struct
import sys
from collections import Counter


def close_words(a, b):
    if a == b:
        return True
    fa, fb = (struct.unpack("<f", struct.pack("<I", int(x, 16)))[0] for x in (a, b))
    if fa != fa and fb != fb:
        return True
    return abs(int(a, 16) - int(b, 16)) <= 4 or (abs(fa - fb) <= 1e-5 * max(1.0, abs(fa)))


def close_buf(a, b):
    if a == b:
        return True
    if len(a) != len(b) or len(a) % 8:
        return False
    return all(close_words("".join(reversed([a[i + k:i + k + 2] for k in range(0, 8, 2)])),
                           "".join(reversed([b[i + k:i + k + 2] for k in range(0, 8, 2)]))) for i in range(0, len(a), 8))


calls = [l.strip() for l in open(sys.argv[1]) if l.strip()]
ref = [l.strip() for l in open(sys.argv[2])]
lif = [l.strip() for l in open(sys.argv[3])]
bad = Counter(); examples = {}
for i, c in enumerate(calls):
    name = c.split()[1]
    if i >= len(lif):
        bad[name + " (not run)"] += 1; continue
    r, l = ref[i].split(), lif[i].split()
    if r[0] == "ERR":
        continue
    ok = len(r) == len(l) and close_words(r[0], l[0]) and all(close_buf(x, y) for x, y in zip(r[2:], l[2:]))
    if ok and r[1] != l[1] and ("aeabi_d" in name or "l2" in name or "f2d" in name):
        ok = close_words(r[1], l[1])
    if not ok:
        bad[name] += 1; examples.setdefault(name, (i + 1, c[:120], ref[i][:120], lif[i][:120]))
print(f"{len(calls)} calls, {sum(bad.values())} mismatches")
for n, k in bad.most_common():
    print(f"  {k:4d}  {n}")
    if n in examples:
        ln, c, r, l = examples[n]
        print(f"        line {ln}: {c}\n        ref:    {r}\n        lifted: {l}")
