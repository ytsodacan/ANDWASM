#!/usr/bin/env python3
"""One command: Clash Royale APK -> the web game (dist/ ready for tools/build_worker.sh and tools/build_site.sh).

Usage:  python3 tools/apk_to_web.py <game.apk> [--work build/apk_web] [--overlay assets_overlay]

Stages (each checks its output before the next one runs; re-running skips finished stages):
  1. extract  tools/extract_apk.py      -> <work>/extract/raw (+ decoded csv/sc)
  2. lift     recomp/build.py           -> <work>/libg/gen (C) and <work>/libg/libg.img   (x86 libg.so from the APK)
  3. compile  recomp/Makefile (emscripten) -> <work>/libg/web/libg.wasm, libg.js, libg.data
  4. pack     tools/pack_dist.py        -> <work>/dist (manifest + hashed chunks, asset pack)
Needs: python3 with numpy and Pillow, and the emscripten SDK at ~/emsdk (or $EMSDK).
"""
import argparse, os, shutil, subprocess, sys, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PY = sys.executable

def run(cmd, cwd=ROOT, env=None):
    print("  $", " ".join(str(c) for c in cmd), flush=True)
    r = subprocess.run([str(c) for c in cmd], cwd=cwd, env=env)
    if r.returncode != 0:
        sys.exit(f"stage failed (exit {r.returncode}): {' '.join(str(c) for c in cmd)}")

def need(path, what):
    if not Path(path).exists():
        sys.exit(f"missing {what}: {path}")

def check_tools():
    try:
        import numpy, PIL  # noqa: F401
    except ImportError as e:
        sys.exit(f"install the Python packages first: pip install numpy pillow  ({e})")
    emsdk = Path(os.environ.get("EMSDK", Path.home() / "emsdk"))
    if not (emsdk / "upstream/emscripten/emcc").exists():
        sys.exit(f"emscripten not found at {emsdk} (set EMSDK=/path/to/emsdk and run emsdk install/activate)")
    return emsdk

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("apk")
    ap.add_argument("--work", default="build/apk_web")
    ap.add_argument("--overlay", default="assets_overlay", help="extra asset files packed over the stock ones (optional)")
    a = ap.parse_args()
    apk = Path(a.apk).resolve(); work = (ROOT / a.work).resolve()
    need(apk, "APK"); work.mkdir(parents=True, exist_ok=True)
    emsdk = check_tools()
    print(f"[1/4] extract {apk.name}")
    ex = work / "extract"
    if not (ex / "raw/lib/x86/libg.so").exists():
        run([PY, "-I", ROOT / "tools/extract_apk.py", apk, ex])
    need(ex / "raw/lib/x86/libg.so", "x86 libg.so inside the APK (is this the 32-bit x86 build?)")
    need(ex / "raw/assets", "assets folder from the APK")
    print("[2/4] lift x86 libg.so to C")
    lib = work / "libg"
    if not (lib / "libg.img").exists():
        run([PY, "-I", ROOT / "recomp/build.py", ex / "raw/lib/x86/libg.so", lib])
    need(lib / "gen", "lifted C sources (build.py output)")
    need(lib / "libg.img", "memory image (build.py output)")
    print("[3/4] compile to WebAssembly")
    if not (lib / "web/libg.wasm").exists():
        env = dict(os.environ, EMSDK=str(emsdk))
        run(["make", "-f", ROOT / "recomp/Makefile", f"OUT={lib.relative_to(ROOT)}", f"ASSETS={ex / 'raw/assets'}", "-j8"], env=env)
    need(lib / "web/libg.wasm", "libg.wasm (compile stage)")
    need(lib / "web/libg.js", "libg.js (compile stage)")
    print("[4/4] pack game files")
    dist = work / "dist"
    if dist.exists(): shutil.rmtree(dist)
    overlay = ROOT / a.overlay
    args = [PY, ROOT / "tools/pack_dist.py", lib / "web", ex / "raw/assets", dist]
    if overlay.exists(): args.append(overlay)
    run(args)
    need(dist / "manifest.json", "manifest.json (pack stage)")
    print(f"\nDone. Web build: {dist}")
    print("Next: sh tools/build_worker.sh (after pointing it at this dist) and tools/build_site.sh")

if __name__ == "__main__":
    main()
