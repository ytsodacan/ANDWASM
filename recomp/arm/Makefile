# Build a lifted armeabi-v7a library (recomp/arm/build_arm.py output) into a browser build.
# Usage: make -f recomp/arm/Makefile OUT=build/lib-<hash> NAME=<library name without .so> ASSETS=<dir> APK=<base.apk> CFG=<andwasm.cfg> -j6
# (recomp/arm/port_apk.py runs this for you)
OUT      ?= build/arm
NAME     ?= libmain
ASSETS   ?=
APK      ?=
CFG      ?=
# WEB: web output folder (one per game when several games share one lifted library)
WEB      ?= $(OUT)/web
RT       := recomp/arm/runtime
EMCC     := $(HOME)/emsdk/upstream/emscripten/emcc
CFLAGS   := -O1 -w -pthread -mtail-call -I$(RT) -I$(OUT)
GEN_SRC  := $(wildcard $(OUT)/gen/*.c)
GEN_OBJ  := $(patsubst $(OUT)/gen/%.c,$(OUT)/obj/%.o,$(GEN_SRC))
RT_SRC   := $(wildcard $(RT)/*.c) $(OUT)/imports_gen.c $(OUT)/table.c $(wildcard $(OUT)/hooks.c)   # hooks.c: per-game --wrap hooks
RT_OBJ   := $(patsubst %.c,$(OUT)/obj/rt_%.o,$(notdir $(RT_SRC)))
EXPORTS  := -sEXPORTED_FUNCTIONS=_main,_malloc,_free,_web_touch,_web_resize,_web_key -sEXPORTED_RUNTIME_METHODS=FS,ccall,ENV
LDFLAGS  := -O1 -pthread -mtail-call -sPTHREAD_POOL_SIZE=8 -sMAXIMUM_MEMORY=2GB -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB \
            -sSTACK_SIZE=8MB -sMAX_WEBGL_VERSION=1 -sFORCE_FILESYSTEM=1 -sEXIT_RUNTIME=0 -lGL $(EXPORTS) --profiling-funcs \
            --preload-file $(OUT)/$(NAME).img@/$(NAME).img $(if $(ASSETS),--preload-file $(ASSETS)@/assets) $(if $(APK),--preload-file $(APK)@/base.apk) $(if $(CFG),--preload-file $(CFG)@/andwasm.cfg)

all: $(WEB)/index.js

gen: $(GEN_OBJ)

$(OUT)/imports_gen.c: recomp/arm/gen_imports.py
	python3 recomp/arm/gen_imports.py $@

# Lifted code: -O2 (keeps guest registers in wasm locals). RING=1 adds the debug call ring.
GEN_CFLAGS := $(subst -O1,-O2,$(CFLAGS)) $(if $(RING),-DRT_RING)
$(OUT)/obj/%.o: $(OUT)/gen/%.c $(RT)/rt.h
	@mkdir -p $(dir $@)
	$(EMCC) $(GEN_CFLAGS) -c $< -o $@

$(OUT)/obj/rt_%.o: $(RT)/%.c $(RT)/rt.h
	@mkdir -p $(dir $@)
	$(EMCC) $(CFLAGS) -c $< -o $@
$(OUT)/obj/rt_%.o: $(OUT)/%.c $(RT)/rt.h
	@mkdir -p $(dir $@)
	$(EMCC) $(CFLAGS) -c $< -o $@

$(WEB)/index.js: $(GEN_OBJ) $(RT_OBJ) recomp/arm/web/index.html
	@mkdir -p $(dir $@)
	$(EMCC) $(LDFLAGS) $(GEN_OBJ) $(RT_OBJ) -o $@
	cp recomp/arm/web/index.html $(WEB)/index.html

.PHONY: all gen

# Headless Node build (real filesystem, no WebGL) for fast boot debugging:
#   ANDWASM_ROOT=<dir with libBBRuntime.img and assets/> node $(OUT)/node/index.js
NODE_RT_OBJ := $(filter-out $(OUT)/obj/rt_main.o,$(RT_OBJ)) $(OUT)/obj/node_main.o $(OUT)/obj/rt_glnull.o
GLES2_H     := $(HOME)/emsdk/upstream/emscripten/cache/sysroot/include/GLES2/gl2.h
$(OUT)/glnull.c: recomp/arm/gen_glnull.py
	python3 recomp/arm/gen_glnull.py $(GLES2_H) $@
$(OUT)/obj/node_main.o: $(RT)/main.c $(RT)/rt.h
	$(EMCC) $(CFLAGS) -DNODE_BUILD -c $< -o $@
$(OUT)/node/index.js: $(GEN_OBJ) $(NODE_RT_OBJ)
	@mkdir -p $(dir $@)
	$(EMCC) -O1 -pthread -mtail-call -sPTHREAD_POOL_SIZE=8 -sALLOW_MEMORY_GROWTH=1 -sINITIAL_MEMORY=512MB -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=8MB \
	  -sNODERAWFS=1 -sEXIT_RUNTIME=1 -sPROXY_TO_PTHREAD=1 --profiling-funcs $(GEN_OBJ) $(NODE_RT_OBJ) -o $@
node: $(OUT)/node/index.js
.PHONY: node
