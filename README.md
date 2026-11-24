# ANDWASM

Drag an Android APK onto the program and get a web build of its native game code (WebAssembly).

```
cd tools/andwasm && cargo build --release
./target/release/andwasm /path/to/game.apk      # or drag the .apk onto the Terminal window
```

The output appears next to the APK as `<name>-web/` (a `manifest.json`, the compiled `.wasm`/`.js`, and the asset chunks).

## What it does
1. Checks the file is an APK that ships an x86 native library (`lib/x86/*.so`).
2. Runs the pipeline (`tools/apk_to_web.py`):
   - extract the APK (`tools/extract_apk.py`)
   - lift the x86 library to C (`recomp/build.py`)
   - compile to WebAssembly with emscripten (`recomp/Makefile`)
   - pack the game files (`tools/pack_dist.py`)

## Requirements
- Rust (`cargo`), Python 3 with `numpy` and `Pillow`, `unzip`
- Emscripten SDK at `~/emsdk` (or set `EMSDK`)

## Limits
- Native-code APKs only. Pure Java/Kotlin apps (Dalvik bytecode) aren't supported.
- Android system APIs the app uses need shims in `recomp/runtime`. The runtime currently has networking and debugging code from the WebRoyale project, which is generic enough to build against, but it isn't yet a general-purpose runtime.

## Layout
- `tools/andwasm/` - the Rust drag-and-drop front end
- `tools/apk_to_web.py` - the build driver
- `recomp/` - the x86 lifter, the emscripten build and the runtime
