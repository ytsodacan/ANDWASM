// Boot harness for lifted cocos2d-x / Buildbox games: plays the part of the Java activity.
// Loads the image, runs constructors and JNI_OnLoad, makes the WebGL context, then drives
// Cocos2dxRenderer.nativeInit / nativeRender every frame and forwards touches.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#include <emscripten/html5.h>
#endif

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
int rt_load(const uint8_t *image, uint32_t len);
void rt_run_init_array(void);
uint32_t rt_new_guest_stack(uint32_t size);
uint32_t jni_env(void), jni_vm(void), jni_str(const char *), jni_class(const char *), jni_object(const char *);
uint32_t jni_array(uint32_t n, uint32_t esz, const char *cls);
uint8_t *jni_array_data(uint32_t a);

int web_width = 1280, web_height = 720;   // landscape default when the page has no size yet
static uint32_t env, renderer;

static uint32_t native(const char *name) {
  uint32_t a = rt_export(name);
  if (!a) LOGI("[boot] missing native %s\n", name);
  return a;
}
static uint32_t fbits(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

// ---- touches (queued from JS, delivered on the render thread) ----
#define MAXQ 256
static struct { int type, id; float x, y; } q[MAXQ]; static volatile int qh, qt;
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void web_touch(int type, int id, float x, float y) {   // type 0 down, 1 move, 2 up, 3 cancel
  int n = (qt + 1) % MAXQ; if (n == qh) return;
  q[qt].type = type; q[qt].id = id; q[qt].x = x; q[qt].y = y; qt = n;
}
// ---- keys (Android key codes, e.g. DPAD 19-23), delivered to Cocos2dxRenderer.nativeKeyEvent ----
static struct { int code, down; } kq[MAXQ]; static volatile int kh, kt;
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void web_key(int code, int down) {
  int n = (kt + 1) % MAXQ; if (n == kh) return;
  kq[kt].code = code; kq[kt].down = down; kt = n;
}
static void deliver_keys(void) {
  static uint32_t f_key;
  if (!f_key) f_key = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeKeyEvent");
  while (kh != kt) {
    int code = kq[kh].code, down = kq[kh].down; kh = (kh + 1) % MAXQ;
    if (f_key) rt_call(f_key, 4, env, renderer, (uint32_t)code, (uint32_t)(down ? 1 : 0));
  }
}
static int resized;
#ifdef __EMSCRIPTEN__
EMSCRIPTEN_KEEPALIVE
#endif
void web_resize(int w, int h) { web_width = w; web_height = h; resized = 1; }

static void deliver_touches(void) {
  static uint32_t f_begin, f_move, f_end, f_cancel;
  if (!f_begin) {
    f_begin = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesBegin");
    f_move = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesMove");
    f_end = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesEnd");
    f_cancel = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeTouchesCancel");
  }
  while (qh != qt) {
    int t = q[qh].type, id = q[qh].id; float x = q[qh].x, y = q[qh].y; qh = (qh + 1) % MAXQ;
    if (t == 0 && f_begin) rt_call(f_begin, 5, env, renderer, (uint32_t)id, fbits(x), fbits(y));
    else if (t == 2 && f_end) rt_call(f_end, 5, env, renderer, (uint32_t)id, fbits(x), fbits(y));
    else if ((t == 1 || t == 3) && (t == 1 ? f_move : f_cancel)) {
      uint32_t ids = jni_array(1, 4, "[I"), xs = jni_array(1, 4, "[F"), ys = jni_array(1, 4, "[F");
      memcpy(jni_array_data(ids), &id, 4); memcpy(jni_array_data(xs), &x, 4); memcpy(jni_array_data(ys), &y, 4);
      rt_call(t == 1 ? f_move : f_cancel, 5, env, renderer, ids, xs, ys);
    }
  }
}

static uint32_t f_render;
int game_frames;
static void frame(void) {
  if (resized) {
    resized = 0;
    uint32_t f = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeOnSurfaceChanged");
    if (f) rt_call(f, 4, env, renderer, (uint32_t)web_width, (uint32_t)web_height);
  }
  deliver_touches();
  deliver_keys();
  rt_call(f_render, 2, env, renderer);
  if (++game_frames == 1) LOGI("[boot] first frame rendered\n");
}

int main(int argc, char **argv) {
  (void)argc; (void)argv;
  const char *rt_fs_root(void);
  char path[512];
  snprintf(path, sizeof path, "%s/data", rt_fs_root()); mkdir(path, 0777);
  snprintf(path, sizeof path, "%s/data/files", rt_fs_root()); mkdir(path, 0777);
  { // The game reads its data folders (fonts, images, ...) from the data path, where the Android app keeps
    // them; link each folder shipped in assets/data there.
    char src[512]; snprintf(src, sizeof src, "%s/assets/data", rt_fs_root());
    DIR *d = opendir(src); struct dirent *e;
    while (d && (e = readdir(d))) {
      if (e->d_name[0] == '.') continue;
      char from[700], to[700]; struct stat st;
      snprintf(from, sizeof from, "%s/%s", src, e->d_name);
      snprintf(to, sizeof to, "%s/data/files/%s", rt_fs_root(), e->d_name);
      if (!stat(from, &st) && S_ISDIR(st.st_mode) && lstat(to, &st)) symlink(from, to);
    }
    if (d) closedir(d);
  }
  snprintf(path, sizeof path, "%s/%s", rt_fs_root(), IMAGE_NAME);
  FILE *f = fopen(path, "rb");
  if (!f) { LOGI("[boot] cannot open %s\n", path); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *img = malloc(n); fread(img, 1, n, f); fclose(f);
  if (rt_load(img, (uint32_t)n)) { LOGI("[boot] load failed\n"); return 1; }
  free(img);
  g_r[13] = rt_new_guest_stack(8u << 20);
  LOGI("[boot] image at %x (%u bytes), %u functions\n", B, IMAGE_SIZE, FUNC_COUNT);

#if defined(__EMSCRIPTEN__) && !defined(NODE_BUILD)
  double cw, ch; emscripten_get_element_css_size("#canvas", &cw, &ch);
  if (cw > 0 && ch > 0) { double dpr = emscripten_get_device_pixel_ratio(); web_width = (int)(cw * dpr); web_height = (int)(ch * dpr); }
  emscripten_set_canvas_element_size("#canvas", web_width, web_height);
  EmscriptenWebGLContextAttributes at; emscripten_webgl_init_context_attributes(&at);
  at.alpha = 0; at.depth = 1; at.stencil = 1; at.antialias = 0; at.majorVersion = 1; at.preserveDrawingBuffer = 0;
  EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &at);
  if (ctx <= 0) { LOGI("[boot] WebGL context failed\n"); return 1; }
  emscripten_webgl_make_context_current(ctx);
  static const char *exts[] = { "OES_vertex_array_object", "OES_element_index_uint", "OES_standard_derivatives", "OES_texture_float",
                                "OES_texture_half_float", "WEBGL_depth_texture", "EXT_texture_filter_anisotropic", "WEBGL_compressed_texture_etc1" };
  for (unsigned i = 0; i < sizeof exts / sizeof *exts; i++) emscripten_webgl_enable_extension(ctx, exts[i]);
#endif

  { void rt_start_watchdog(void); rt_start_watchdog(); }
  rt_run_init_array();
  LOGI("[boot] constructors done\n");
  if (getenv("SELFTEST")) { void rt_selftest(void), rt_selftest_zip(void), rt_selftest_calls(void); rt_selftest(); rt_selftest_zip(); rt_selftest_calls(); if (getenv("SELFTEST_CALLS")) return 0; }
  env = jni_env();
  uint32_t onload = native("JNI_OnLoad");
  if (onload) LOGI("[boot] JNI_OnLoad -> %x\n", rt_call(onload, 2, jni_vm(), 0));

  uint32_t helper = jni_class("org/cocos2dx/lib/Cocos2dxHelper");
  uint32_t activity = jni_object("org/buildbox/player/BBRuntimeActivity");
  renderer = jni_object("org/cocos2dx/lib/Cocos2dxRenderer");
  uint32_t fn;
  if ((fn = native("Java_org_buildbox_player_BBRuntimeActivity_setDataPath"))) rt_call(fn, 3, env, activity, jni_str("/data/files/"));
  if ((fn = native("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetApkPath"))) rt_call(fn, 3, env, helper, jni_str("/base.apk"));
  if ((fn = native("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetAudioDeviceInfo"))) rt_call(fn, 5, env, helper, 0u, 48000u, 192u);
  if ((fn = native("Java_org_cocos2dx_lib_Cocos2dxHelper_nativeSetContext")))
    rt_call(fn, 4, env, helper, jni_object("android/content/Context"), jni_object("android/content/res/AssetManager"));
#ifdef NODE_BUILD
  if (!getenv("NODE_INIT")) { LOGI("[boot] node build: Java-side setup done; skipping nativeInit (needs WebGL)\n"); return 0; }
#endif
  LOGI("[boot] nativeInit %dx%d\n", web_width, web_height);
  if ((fn = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeInit"))) rt_call(fn, 4, env, renderer, (uint32_t)web_width, (uint32_t)web_height);
  f_render = native("Java_org_cocos2dx_lib_Cocos2dxRenderer_nativeRender");
  LOGI("[boot] running\n");
#if defined(__EMSCRIPTEN__) && !defined(NODE_BUILD)
  emscripten_set_main_loop(frame, 0, 0);
#else
  LOGI("[boot] node build: stopping before rendering (no WebGL)\n");
#endif
  return 0;
}
