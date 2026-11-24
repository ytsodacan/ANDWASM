#!/usr/bin/env python3
"""Unpack a Clash Royale (Supercell) APK and decode its CSV + .sc assets.

Usage: python3 -I tools/extract_apk.py <apk> <out_dir>

Layout produced:
  out/raw/          verbatim APK contents (zip extract)
  out/csv/          decompressed CSVs (csv_logic, csv_client, locations...)
  out/sc/           decompressed .sc SWF-like asset blobs (+ *_tex.sc textures)
  out/report.txt    directory/native-lib inventory
Supercell LZMA: 5-byte props + 4-byte LE size (standard LZMA_ALONE uses 8 bytes),
so we pad the size field to 8 bytes before handing it to lzma.
.sc v1/v2: b'SC' + u32be version + u32be hashlen + hash + LZMA payload.
"""
import lzma, struct, sys, zipfile, collections
from pathlib import Path

def sc_lzma(data: bytes) -> bytes:
    fixed = data[:9] + b"\x00\x00\x00\x00" + data[9:]
    return lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(fixed)

def decode(data: bytes) -> tuple[bytes, str]:
    if data[:2] == b"SC":
        ver, hlen = struct.unpack(">II", data[2:10])
        body = data[10 + hlen:]
        if ver == 3 or body[:4] == b"\x28\xb5\x2f\xfd":
            return data, f"sc-v{ver}-zstd(skipped)"
        return sc_lzma(body), f"sc-v{ver}-lzma"
    if data[:1] == b"\x5d":
        return sc_lzma(data), "lzma"
    return data, "plain"

def main(apk: str, out: str):
    out = Path(out); raw = out / "raw"
    with zipfile.ZipFile(apk) as z:
        z.extractall(raw)
        names = z.namelist()
    rep = [f"APK: {apk}", f"files: {len(names)}", ""]
    tops = collections.Counter("/".join(n.split("/")[:2]) for n in names)
    rep += [f"{c:5d}  {d}" for d, c in tops.most_common()]
    rep += ["", "Native libs:"]
    for p in sorted((raw / "lib").rglob("*.so")):
        rep.append(f"  {p.relative_to(raw)}  {p.stat().st_size:,} bytes")
    stats = collections.Counter()
    for src in sorted((raw / "assets").rglob("*")):
        if src.suffix not in (".csv", ".sc"): continue
        rel = src.relative_to(raw / "assets")
        dst = out / ("csv" if src.suffix == ".csv" else "sc") / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        try:
            data, kind = decode(src.read_bytes())
        except Exception as e:
            data, kind = src.read_bytes(), f"error:{e}"
        dst.write_bytes(data); stats[kind] += 1
    rep += ["", "Decode stats:"] + [f"  {k}: {v}" for k, v in stats.items()]
    (out / "report.txt").write_text("\n".join(rep) + "\n")
    print("\n".join(rep))

if __name__ == "__main__":
    main(*sys.argv[1:3])
