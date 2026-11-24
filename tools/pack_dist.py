#!/usr/bin/env python3
"""Build dist/ for git hosting: gzip'd wasm + libg.data, the glue script, and the game assets as a
chunked pack, plus manifest.json (sha256 of every file). The page (recomp/web/index.html) downloads
these through the server's /_git/ route and keeps them in the browser's Cache Storage, so each file is
fetched once per version.

Usage: python3 tools/pack_dist.py [web_dir=build/libg/web] [assets_dir=extracted_retro/x/raw/assets] [out=dist] [overlay=assets_overlay]
Files under overlay/ (same relative paths, e.g. sc/ui.sc) replace or add to the stock assets; they come
from tools/sc/build_emotes.py (image emotes baked into the game's emote panel).
Asset pack stream: repeated [u32 path_len][path][u32 data_len][data] (big-endian), cut into 14 MiB
parts, each gzip'd on its own so parts download and decompress independently."""
import gzip, hashlib, json, os, re, struct, sys, time

web = sys.argv[1] if len(sys.argv) > 1 else "build/libg/web"
assets = sys.argv[2] if len(sys.argv) > 2 else "extracted_retro/x/raw/assets"
out = sys.argv[3] if len(sys.argv) > 3 else "dist"
overlay = sys.argv[4] if len(sys.argv) > 4 else "assets_overlay"
PART = 14 << 20   # parts stay under 20 MB gzipped: jsDelivr refuses bigger files
os.makedirs(out, exist_ok=True)
for f in os.listdir(out): os.remove(os.path.join(out, f))
files = []
def emit(name, data, raw_size, kind):
    sha = hashlib.sha256(data).hexdigest()
    # Stored under a content-addressed name (libg.wasm.gz -> libg.wasm.<sha8>.gz) so CDNs (jsDelivr) can never serve a stale copy;
    # "name" stays the logical name the page uses, "path" is the file on disk.
    path = name if kind == "glue" else re.sub(r"(\.gz)?$", lambda m: "." + sha[:8] + (m.group(1) or ""), name, count=1)
    open(os.path.join(out, path), "wb").write(data)
    files.append({"name": name, "path": path, "sha256": sha, "size": len(data), "raw": raw_size, "kind": kind})
    print(f"  {path:34s} {len(data)/1048576:7.1f} MiB")
def gz(b): return gzip.compress(b, 9, mtime=0)

for n, kind in (("libg.wasm", "wasm"), ("libg.data", "data")):
    b = open(os.path.join(web, n), "rb").read(); emit(n + ".gz", gz(b), len(b), kind)
b = open(os.path.join(web, "libg.js"), "rb").read()
HEAP = b"/** @type {!Uint8Array} */ var HEAPU8;"
assert HEAP in b
b = b.replace(HEAP, HEAP + b'\nModule["__heap"] = () => (growMemViews(), HEAPU8);', 1)   # lets the page write typed chat text into game memory
emit("libg.js", b, len(b), "glue")

def tree(top):
    if not os.path.isdir(top): return {}
    return {os.path.relpath(os.path.join(r, n), top).replace(os.sep, "/"): os.path.join(r, n) for r, _, ns in os.walk(top) for n in ns}
src = tree(assets); over = tree(overlay); src.update(over)
if over: print(f"  overlay: {', '.join(sorted(over))}")
stream = bytearray(); count = 0
# stock files first (stable chunk boundaries), overlay files last: editing the overlay only changes the final parts
for rel in sorted(src, key=lambda r: (r in over, r)):
        d = open(src[rel], "rb").read(); p = rel.encode()
        stream += struct.pack(">I", len(p)) + p + struct.pack(">I", len(d)) + d; count += 1
for i in range(0, len(stream), PART):
    chunk = bytes(stream[i:i + PART]); emit(f"assets.{i // PART:03d}.gz", gz(chunk), len(chunk), "assets")
manifest = {"version": hashlib.sha256("".join(f["sha256"] for f in files).encode()).hexdigest()[:16],
            "built": int(time.time()), "assetFiles": count, "assetBytes": len(stream), "files": files}
json.dump(manifest, open(os.path.join(out, "manifest.json"), "w"), indent=1)
print(f"{count} asset files, version {manifest['version']}, total {sum(f['size'] for f in files)/1048576:.0f} MiB")
