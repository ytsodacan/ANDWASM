# ANDWASM

Turn an Android game's native code into a web build. ANDWASM statically translates the game's machine code
(ARM or x86) to C, compiles it to WebAssembly with Emscripten, and runs it on a small runtime that stands in
for Android (libc, threads, OpenGL ES → WebGL, assets, audio, touch, a stand-in Java layer).

- **ARM (armeabi-v7a)** — the common case. Takes `.apk`, `.xapk` and `.apkm` (split bundles). Tested on
  Buildbox (cocos2d-x + SpiderMonkey) games: they boot, render, play sound and take keyboard and touch input.
- **x86** — the original pipeline, for APKs that ship `lib/x86`.

---

## 1. Set up (once)

You need macOS or Linux with:

| Tool | Install |
|---|---|
| Python 3.10+ | system Python is fine |
| Python packages | `pip3 install capstone pyelftools unicorn numpy pillow` |
| Emscripten SDK in `~/emsdk` | `git clone https://github.com/emscripten-core/emsdk ~/emsdk && cd ~/emsdk && ./emsdk install latest && ./emsdk activate latest` |
| Rust (only for the drag-and-drop app) | https://rustup.rs |
| `unzip` | usually preinstalled |

Then get ANDWASM:

```bash
git clone https://github.com/ytsodacan/ANDWASM.git
```

```bash
cd ANDWASM/tools/andwasm && cargo build --release
```

---

## 2. Port a game

**Easiest:** run the app and drag the `.apk` / `.xapk` / `.apkm` onto the Terminal window, then press Enter.

```bash
./tools/andwasm/target/release/andwasm
```

**Or one command** (from the repository root):

```bash
python3 recomp/arm/port_apk.py ~/Downloads/mygame.xapk --name mygame
```

What happens:

1. The package is unpacked; the base APK and the armeabi-v7a native library are found automatically.
2. The library is lifted to C (`recomp/arm/build_arm.py`) and compiled to WebAssembly (`recomp/arm/Makefile`).
   This is the slow part: about 2–3 minutes to lift and ~20 minutes to compile a 20 MB library.
3. Builds are cached by the library's content hash (`build/lib-<hash>`). **Another game that ships the same
   engine library reuses the cached build and is ready in about a minute** — only its data is linked.
4. Engine profiles are applied for you. For Buildbox games the script-engine JIT is switched off (scripts run in
   the interpreter), the on-screen joystick is hidden for keyboard play, and the data-archive key is read from
   the app's own code.
5. The finished page is written to `build/games/mygame/web/` (and next to your file as `mygame-web/` when you
   use the drag-and-drop app).

---

## 3. Play it locally

Threaded WebAssembly needs two HTTP headers (`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`). The included server sends them:

```bash
python3 recomp/arm/web/serve.py build/games/mygame/web 8000
```

Open http://localhost:8000/ and click once (browsers only allow sound after a click or key press).

### Controls

| Input | What the game gets |
|---|---|
| Mouse / touch | Android touches (multi-touch) |
| Arrow keys or WASD | A virtual joystick (a held touch pushed in that direction) **and** D-pad key events |
| Enter / Space | D-pad centre |

If a game's joystick lives at a fixed spot, put the virtual stick there: `?joy=0.15,0.8` (x and y as fractions
of the screen). The screen is landscape 16:9 and follows the window size.

---

## 4. Put it online

Upload the four files in `web/` (`index.html`, `index.js`, `index.wasm`, `index.data`) to any static host that
can send the two headers above, for example:

- **nginx**:
  ```
  add_header Cross-Origin-Opener-Policy same-origin;
  add_header Cross-Origin-Embedder-Policy require-corp;
  ```
- **Cloudflare Pages / Netlify**: a `_headers` file next to `index.html`:
  ```
  /*
    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
  ```

`index.wasm` is ~70 MB and `index.data` holds the game's assets, so enable gzip/brotli. Only publish games you
have the right to distribute.

---

## 5. When something doesn't work

Add switches to the page URL as `?env=NAME` (several allowed):

| Switch | Logs |
|---|---|
| `FILE_TRACE` | every file and asset the game opens (missing files show up here) |
| `GL_DEBUG` | the GL call behind every WebGL error |
| `SCANF_TRACE` | `sscanf` calls (data-file parsing) |
| `SHOW_JOYSTICK` | keeps the on-screen joystick visible |

The page keeps a searchable log in `window.__andwasmLog` (browser console). A crash prints the failing guest
address; `RING=1` in the build (`make ... RING=1`) adds a call trail and a watchdog that shows where a hung game
is spinning.

**Headless runs** (no browser, much faster to iterate): the `node` Makefile target builds the same game for
Node.js with stub graphics:

```bash
make -f recomp/arm/Makefile OUT=build/lib-<hash> NAME=libBBRuntime node
```

```bash
ANDWASM_ROOT=<folder with the .img, assets/ and andwasm.cfg> NODE_INIT=1 node build/lib-<hash>/node/index.js
```

**Checking the translator against real ARM**: `recomp/arm/difftest/` runs the same function calls on Unicorn
(an ARM CPU emulator) and in the lifted build and compares the results bit for bit.

```bash
python3 recomp/arm/difftest/gen_tests.py <lib.so> calls.txt
```

```bash
python3 recomp/arm/difftest/run_unicorn.py <lib.so> calls.txt ref.txt
```

```bash
SELFTEST=1 SELFTEST_CALLS=$PWD/calls.txt SELFTEST_OUT=$PWD/lifted.txt ANDWASM_ROOT=... node build/lib-<hash>/node/index.js
```

```bash
python3 recomp/arm/difftest/compare.py calls.txt ref.txt lifted.txt
```

---

## 6. Supporting a new game or engine

Most work is in the stand-in Android layer, not the translator:

- **Java calls** the game makes through JNI land in `java_call()` in `recomp/arm/runtime/android.c`. Unhandled
  ones are logged once as `[jni] unhandled Class.method(sig)`; add a case for the ones that matter.
- **Constant values** returned by simple Java getters go in `andwasm.cfg` (`Class.method=value`, next to the
  build) instead of code.
- **Replace a native function** with a fixed return value: `build_arm.py --stub SYMBOL=VALUE`.
- **Run host code around a native function**: `build_arm.py --wrap SYMBOL`, then write `hook_<addr>()` in the
  build's `hooks.c` (it can call `F_<addr>_orig()`).
- **Missing libc / system functions** show up as `unimplemented import: name`; add them to
  `recomp/arm/runtime/libc.c` or the generated wrappers (`recomp/arm/gen_imports.py`).

---

## How it works

- `recomp/arm/elf_arm.py` reads the ELF: segments, relocations, imports, functions (symbols, `.ARM.exidx`) and
  the ARM/Thumb/data mapping symbols.
- `recomp/arm/lift_arm.py` turns each function into a C function over the guest registers: ARMv7 and Thumb-2,
  VFP and NEON, conditional execution, jump tables, computed gotos, linker veneers, shared code entered in the
  middle of a function, returns that skip frames, and WebAssembly tail calls for jumps between functions.
- `recomp/arm/build_arm.py` finds every function (iterating call targets and PC-relative code pointers until
  nothing new turns up), lifts in parallel and writes the dispatch, relocation and import tables.
- `recomp/arm/runtime/` is the stand-in Android: AAPCS calls, bionic libc and pthreads on Emscripten threads,
  JNI, AAssetManager, WebGL fixes, browser audio, input and the boot sequence of a cocos2d-x activity.

## Limits

- 32-bit ARM (armeabi-v7a) and x86 only; arm64-only apps aren't supported yet.
- Java/Kotlin code is not run; the Java side is a stand-in that answers the calls native code makes.
- Engines that generate machine code at run time (JITs) must run without it; Buildbox's is switched off
  automatically.
- No networking (sockets fail), no in-app purchases or ads, no Google Play services.
- Audio covers the cocos2d-x Java helpers (music and effects); OpenSL ES is not implemented.

## Layout

- `tools/andwasm/` — the drag-and-drop app (Rust)
- `recomp/arm/` — ARM lifter, runtime, web page, porter (`port_apk.py`) and tests (`difftest/`)
- `tools/apk_to_web.py`, `recomp/*.py`, `recomp/runtime/` — the x86 pipeline
- `web/` — generic loader for x86 builds
