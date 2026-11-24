# Incremental build of the recompiled libg.so → libg.js/libg.wasm
# Usage: make -f recomp/Makefile -j6        (from the project root)
#   1. python3 -I recomp/build.py  regenerates build/libg/gen/*.c (only rewrites changed chunks)
#   2. only changed chunks are recompiled; objects are cached in build/libg/obj
OUT      := build/libg
RT       := recomp/runtime
EMCC     := $(HOME)/emsdk/upstream/emscripten/emcc
CFLAGS   := -O1 -w -pthread -I$(RT) -I$(OUT)
GEN_SRC  := $(wildcard $(OUT)/gen/*.c)
GEN_OBJ  := $(patsubst $(OUT)/gen/%.c,$(OUT)/obj/%.o,$(GEN_SRC))
RT_SRC   := $(RT)/rt.c $(RT)/hle.c $(RT)/jni.c $(RT)/fmod_null.c $(RT)/threads.c $(RT)/net.c $(OUT)/hle_gen.c $(OUT)/table.c
RT_OBJ   := $(patsubst %.c,$(OUT)/obj/rt_%.o,$(notdir $(RT_SRC)))
ASSETS   ?= extracted_retro/x/raw/assets
# By default the assets are NOT baked into libg.data: the page downloads them from the git-hosted
# dist/ pack (tools/pack_dist.py) and caches them. Set PRELOAD_ASSETS=1 to bundle them instead.
THREADS  := -sEXPORTED_FUNCTIONS=_main,_free,_malloc,_web_set_touch,_web_touch_count -pthread -sPTHREAD_POOL_SIZE=16 -sMAXIMUM_MEMORY=2GB
LDFLAGS  := -O1 $(THREADS) -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB -sSTACK_SIZE=8MB -sMAX_WEBGL_VERSION=1 \
            -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0 -lGL -lidbfs.js -sEXPORTED_RUNTIME_METHODS=FS,IDBFS,addRunDependency,removeRunDependency --profiling-funcs \
            --preload-file $(OUT)/libg.img@/libg.img $(if $(PRELOAD_ASSETS),--preload-file $(ASSETS)@/assets)

all: $(OUT)/web/libg.js

# Rebuild every object when the compiler flags change.
FLAGS_STAMP := $(OUT)/obj/.flags
$(shell mkdir -p $(OUT)/obj; echo '$(CFLAGS)' | cmp -s - $(FLAGS_STAMP) || echo '$(CFLAGS)' > $(FLAGS_STAMP))

$(OUT)/hle_gen.c: recomp/gen_hle.py
	python3 -I recomp/gen_hle.py $@

# Very large generated files (one lifted function can be several MB of C) compile at -O0: -O1 takes hours on them.
BIG_SRC := $(shell find $(OUT)/gen -name '*.c' -size +4M 2>/dev/null)
BIG_OBJ := $(patsubst $(OUT)/gen/%.c,$(OUT)/obj/%.o,$(BIG_SRC))
$(BIG_OBJ): $(OUT)/obj/%.o: $(OUT)/gen/%.c $(RT)/rt.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(EMCC) $(subst -O1,-O0,$(CFLAGS)) -Wno-error=int-conversion -c $< -o $@

$(OUT)/obj/%.o: $(OUT)/gen/%.c $(RT)/rt.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(EMCC) $(CFLAGS) -Wno-error=int-conversion -c $< -o $@

$(OUT)/obj/rt_%.o: $(RT)/%.c $(RT)/rt.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(EMCC) $(CFLAGS) -Wno-error=int-conversion -c $< -o $@
$(OUT)/obj/rt_%.o: $(OUT)/%.c $(RT)/rt.h $(FLAGS_STAMP)
	@mkdir -p $(dir $@)
	$(EMCC) $(CFLAGS) -Wno-error=int-conversion -c $< -o $@

# The browser build compiles hle.c with WEB_BUILD (WebGL context + rAF loop).
$(OUT)/obj/web_hle.o: $(RT)/hle.c $(RT)/rt.h $(FLAGS_STAMP)
	$(EMCC) $(CFLAGS) -DWEB_BUILD -c $< -o $@
WEB_RT_OBJ := $(filter-out $(OUT)/obj/rt_hle.o,$(RT_OBJ)) $(OUT)/obj/web_hle.o
$(OUT)/web/libg.js: $(GEN_OBJ) $(WEB_RT_OBJ) recomp/web/index.html
	@mkdir -p $(dir $@)
	$(EMCC) $(LDFLAGS) $(GEN_OBJ) $(WEB_RT_OBJ) -o $@
	cp recomp/web/index.html $(OUT)/web/index.html

.PHONY: all

# Headless Node build: real filesystem (NODERAWFS) + null GL, for driving boot without a browser.
NODE_OBJ := $(OUT)/obj/rt_gl_null.o
$(OUT)/node/libg.js: $(GEN_OBJ) $(RT_OBJ) $(NODE_OBJ)
	@mkdir -p $(dir $@)
	$(EMCC) -O1 $(THREADS) -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB -sSTACK_SIZE=8MB -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sPROXY_TO_PTHREAD=1 --profiling-funcs $^ -o $@
node: $(OUT)/node/libg.js
.PHONY: node
