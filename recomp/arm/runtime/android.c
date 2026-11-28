// Android side for lifted cocos2d-x / Buildbox games: a host-implemented JNI environment (JNIEnv/JavaVM
// whose functions are host thunks), AAssetManager on the /assets folder, OpenSL ES (unavailable for now)
// and GLES extras WebGL lacks.
#include "rt.h"
#pragma clang diagnostic ignored "-Wint-conversion"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
const char *rt_fs_root(void);
static const char *gs(uint32_t a) { return (const char *)P(a); }

// ------------------------------------------------------------------ Java objects
// Every jobject handed to the guest is the address of one of these (guest memory == wasm memory).
enum { K_CLASS = 0x4A430001, K_STRING, K_ARRAY, K_OBJECT, K_METHOD, K_FIELD };
typedef struct { uint32_t kind; char *cls; char *str; uint32_t len, esz; uint8_t *data; } jobj;
typedef struct { uint32_t kind; char *cls, *name, *sig; int is_static; } jmeth;

static uint32_t mk(uint32_t kind, const char *cls) { jobj *o = calloc(1, sizeof *o); o->kind = kind; o->cls = strdup(cls ? cls : "java/lang/Object"); return (uint32_t)(uintptr_t)o; }
static jobj *J(uint32_t a) { jobj *o = (jobj *)P(a); return (a && (o->kind >= K_CLASS && o->kind <= K_FIELD)) ? o : NULL; }
uint32_t jni_str(const char *s) { uint32_t a = mk(K_STRING, "java/lang/String"); jobj *o = J(a); o->str = strdup(s ? s : ""); o->len = strlen(o->str); return a; }
uint32_t jni_class(const char *name) { return mk(K_CLASS, name); }
uint32_t jni_object(const char *cls) { return mk(K_OBJECT, cls); }
uint32_t jni_array(uint32_t n, uint32_t esz, const char *cls) {
  uint32_t a = mk(K_ARRAY, cls); jobj *o = J(a); o->len = n; o->esz = esz; o->data = calloc(n ? n : 1, esz); return a;
}
uint8_t *jni_array_data(uint32_t a) { jobj *o = J(a); return o ? o->data : NULL; }

// ------------------------------------------------------------------ simple key-value store (SharedPreferences / UserDefault)
#define MAX_KV 1024
static struct { char *k, *v; } kv[MAX_KV]; static int nkv;
static const char *kv_get(const char *k) { for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, k)) return kv[i].v; return NULL; }
static void kv_set(const char *k, const char *v) {
  for (int i = 0; i < nkv; i++) if (!strcmp(kv[i].k, k)) { free(kv[i].v); kv[i].v = strdup(v); return; }
  if (nkv < MAX_KV) { kv[nkv].k = strdup(k); kv[nkv].v = strdup(v); nkv++; }
}

// ------------------------------------------------------------------ argument decoding for Call*Method
typedef struct { int mode; uint32_t w; uint32_t p; } jargs;   // 0: varargs words, 1: va_list, 2: jvalue[]
static uint32_t ja32(jargs *a) {
  if (a->mode == 0) { uint32_t w = a->w++; return ARGW(w); }
  if (a->mode == 1) { uint32_t v = M32(a->p); a->p += 4; return v; }
  uint32_t v = M32(a->p); a->p += 8; return v;
}
static uint64_t ja64(jargs *a) {
  if (a->mode == 0) { if (a->w & 1) a->w++; uint64_t v = (uint64_t)ARGW(a->w) | ((uint64_t)ARGW(a->w + 1) << 32); a->w += 2; return v; }
  if (a->mode == 1) { a->p = (a->p + 7) & ~7u; uint64_t v = M64(a->p); a->p += 8; return v; }
  uint64_t v = M64(a->p); a->p += 8; return v;
}
#define MAXA 12
typedef struct { int n; char t[MAXA]; uint64_t v[MAXA]; } jcall;   // decoded arguments; t = JNI type letter
static void decode(const char *sig, jargs *a, jcall *c) {
  c->n = 0; const char *s = sig ? strchr(sig, '(') : NULL; if (!s) return; s++;
  while (*s && *s != ')' && c->n < MAXA) {
    char t = *s; int arr = 0;
    while (*s == '[') { arr = 1; s++; }
    if (*s == 'L') { while (*s && *s != ';') s++; }
    if (*s) s++;
    if (arr) t = 'L';
    uint64_t v;
    if (t == 'J') v = ja64(a);
    else if (t == 'D' || t == 'F') {
      if (a->mode == 2) { v = t == 'F' ? M32(a->p) : M64(a->p); a->p += 8; }
      else { uint64_t q = ja64(a); double d; memcpy(&d, &q, 8); if (t == 'F') { float f = (float)d; uint32_t u; memcpy(&u, &f, 4); v = u; } else v = q; }
    } else v = ja32(a);
    c->t[c->n] = t == '[' ? 'L' : t; c->v[c->n++] = v;
  }
}
static const char *astr(jcall *c, int i) { jobj *o = i < c->n ? J((uint32_t)c->v[i]) : NULL; return o && o->str ? o->str : ""; }


// ------------------------------------------------------------------ audio (cocos2d-x Java helpers -> browser audio)
// Music: one <audio> element fed from the file's bytes. Effects: decoded WebAudio buffers.
// Browsers start audio only after a user gesture; playback that was requested earlier starts on the first tap.
#if defined(__EMSCRIPTEN__) && !defined(NODE_BUILD)
#include <emscripten.h>
static const char *audio_path(const char *p, char *buf, size_t n) {
  const char *cands[] = { "/assets/%s", "/assets/data/%s", "/data/files/%s", "%s" };
  if (!strncmp(p, "assets/", 7)) p += 7;
  for (int i = 0; i < 4; i++) { snprintf(buf, n, cands[i], p); FILE *f = fopen(buf, "rb"); if (f) { fclose(f); return buf; } }
  return NULL;
}
static void audio_init(void) { static int done; if (done++) return; MAIN_THREAD_EM_ASM({ if (window.__awInit) window.__awInit(); }); }
static void music_play(const char *path, int loop) {
  char b[600]; const char *p = audio_path(path, b, sizeof b); if (!p) { LOGI("[audio] music not found: %s\n", path); return; }
  audio_init(); MAIN_THREAD_EM_ASM({ if (window.__awMusicPlay) window.__awMusicPlay(UTF8ToString($0), $1); }, p, loop);
}
static void music_cmd(int cmd, float v) { audio_init(); MAIN_THREAD_EM_ASM({ if (window.__awMusicCmd) window.__awMusicCmd($0, $1); }, cmd, v); }
static int music_playing(void) { audio_init(); return MAIN_THREAD_EM_ASM_INT({ return window.__awMusicPlaying ? window.__awMusicPlaying() : 0; }); }
static int effect_play(const char *path, int loop, float pitch, float gain) {
  char b[600]; const char *p = audio_path(path, b, sizeof b); if (!p) { LOGI("[audio] effect not found: %s\n", path); return 0; }
  audio_init(); return MAIN_THREAD_EM_ASM_INT({ return window.__awEffectPlay ? window.__awEffectPlay(UTF8ToString($0), $1, $2, $3) : 0; }, p, loop, pitch, gain);
}
static void effect_cmd(int cmd, int id, float v) { audio_init(); MAIN_THREAD_EM_ASM({ if (window.__awEffectCmd) window.__awEffectCmd($0, $1, $2); }, cmd, id, v); }
static void effect_preload(const char *path) {
  char b[600]; const char *p = audio_path(path, b, sizeof b); if (!p) return;
  audio_init(); MAIN_THREAD_EM_ASM({ if (window.__awEffectPreload) window.__awEffectPreload(UTF8ToString($0)); }, p);
}
#else
static void music_play(const char *p, int l) { (void)p; (void)l; }
static void music_cmd(int c, float v) { (void)c; (void)v; }
static int music_playing(void) { return 0; }
static int effect_play(const char *p, int l, float a, float b) { (void)p; (void)l; (void)a; (void)b; return 0; }
static void effect_cmd(int c, int i, float v) { (void)c; (void)i; (void)v; }
static void effect_preload(const char *p) { (void)p; }
#endif
static float fval(jcall *c, int i) { float f; uint32_t u = i < c->n ? (uint32_t)c->v[i] : 0; memcpy(&f, &u, 4); return f; }

// ------------------------------------------------------------------ Java method implementations
// Returns the 64-bit raw result. Unknown methods log once and return 0 / null.
extern int web_width, web_height;
static uint64_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
// Per-game constants (andwasm.cfg next to the image): lines "Class.method=value" give what simple Java
// getters in the original app return; the build extracts them from the app's dex.
static struct { char *k, *v; } cfg[256]; static int ncfg = -1;
const char *rt_fs_root(void);
static const char *cfg_get(const char *key) {
  if (ncfg < 0) {
    ncfg = 0; char p[512]; snprintf(p, sizeof p, "%s/andwasm.cfg", rt_fs_root());
    FILE *f = fopen(p, "r"); char line[2048];
    while (f && fgets(line, sizeof line, f) && ncfg < 256) {
      char *eq = strchr(line, '='); if (!eq || line[0] == '#') continue;
      *eq = 0; char *v = eq + 1; v[strcspn(v, "\r\n")] = 0;
      cfg[ncfg].k = strdup(line); cfg[ncfg].v = strdup(v); ncfg++;
    }
    if (f) fclose(f);
  }
  for (int i = 0; i < ncfg; i++) if (!strcmp(cfg[i].k, key)) return cfg[i].v;
  return NULL;
}
static uint64_t java_call(const char *cls, const char *name, const char *sig, uint32_t self, jcall *c) {
  const char *k = strrchr(cls, '/'); k = k ? k + 1 : cls;
  char ret = sig ? *(strchr(sig, ')') + 1) : 'V';
  { char key[160]; snprintf(key, sizeof key, "%s.%s", k, name); const char *v = cfg_get(key);
    if (v) { if (ret == 'L') return jni_str(v); if (ret == 'Z' || ret == 'I') return (uint32_t)atoi(v); } }
  #define IS(n) (!strcmp(name, n))
  if (IS("getCocos2dxWritablePath") || IS("getWritablePath")) return jni_str("/data/files");
  if (IS("getCocos2dxPackageName") || IS("getPackageName")) { const char *pk = cfg_get("package"); return jni_str(pk ? pk : "com.andwasm.app"); }
  if (IS("getCurrentLanguage") || IS("getLanguage")) return jni_str("en");
  if (IS("getCurrentLanguageCode")) return jni_str("en");
  if (IS("getDeviceModel")) return jni_str("ANDWASM");
  if (IS("getDPI")) return 320;
  if (IS("getVersion") || IS("getAppVersion")) return jni_str("1.0");
  if (IS("getAssetManager")) return jni_object("android/content/res/AssetManager");
  if (IS("isActivityVisible")) return 1;
  if (IS("getStringForKey")) { const char *v = kv_get(astr(c, 0)); return jni_str(v ? v : astr(c, 1)); }
  if (IS("getIntegerForKey")) { const char *v = kv_get(astr(c, 0)); return v ? (uint32_t)atoi(v) : (uint32_t)c->v[1]; }
  if (IS("getBoolForKey")) { const char *v = kv_get(astr(c, 0)); return v ? (uint32_t)atoi(v) : (uint32_t)c->v[1]; }
  if (IS("getFloatForKey")) { const char *v = kv_get(astr(c, 0)); return v ? f2u((float)atof(v)) : c->v[1]; }
  if (IS("getDoubleForKey")) { const char *v = kv_get(astr(c, 0)); if (!v) return c->v[1]; double d = atof(v); uint64_t q; memcpy(&q, &d, 8); return q; }
  if (IS("setStringForKey")) { kv_set(astr(c, 0), astr(c, 1)); return 0; }
  if (IS("setIntegerForKey") || IS("setBoolForKey")) { char b[32]; snprintf(b, sizeof b, "%d", (int)c->v[1]); kv_set(astr(c, 0), b); return 0; }
  if (IS("setFloatForKey")) { float f; uint32_t u = (uint32_t)c->v[1]; memcpy(&f, &u, 4); char b[64]; snprintf(b, sizeof b, "%.9g", f); kv_set(astr(c, 0), b); return 0; }
  if (IS("setDoubleForKey")) { double d; memcpy(&d, &c->v[1], 8); char b[64]; snprintf(b, sizeof b, "%.17g", d); kv_set(astr(c, 0), b); return 0; }
  if (IS("deleteValueForKey")) return 0;
  if (IS("getSettingsValue") || IS("getSettingsValueNative")) { const char *v = kv_get(astr(c, 0)); return jni_str(v ? v : ""); }
  if (IS("toString")) return jni_str(k);
  // cocos2d-x audio (Cocos2dxHelper / Cocos2dxMusic / Cocos2dxSound)
  if (IS("playBackgroundMusic")) { music_play(astr(c, 0), c->n > 1 ? (int)c->v[1] : 1); return 0; }
  if (IS("preloadBackgroundMusic")) return 0;
  if (IS("stopBackgroundMusic")) { music_cmd(0, 0); return 0; }
  if (IS("pauseBackgroundMusic")) { music_cmd(1, 0); return 0; }
  if (IS("resumeBackgroundMusic")) { music_cmd(2, 0); return 0; }
  if (IS("rewindBackgroundMusic")) { music_cmd(4, 0); return 0; }
  if (IS("setBackgroundMusicVolume")) { music_cmd(3, fval(c, 0)); return 0; }
  if (IS("isBackgroundMusicPlaying")) return music_playing();
  if (IS("getBackgroundMusicVolume")) return f2u(1.0f);
  if (IS("playEffect")) return (uint32_t)effect_play(astr(c, 0), c->n > 1 ? (int)c->v[1] : 0, c->n > 2 ? fval(c, 2) : 1.0f, c->n > 4 ? fval(c, 4) : 1.0f);
  if (IS("stopEffect") || IS("unloadEffect") && 0) { effect_cmd(0, (int)c->v[0], 0); return 0; }
  if (IS("stopAllEffects")) { effect_cmd(1, 0, 0); return 0; }
  if (IS("pauseAllEffects")) { effect_cmd(4, 0, 0); return 0; }
  if (IS("resumeAllEffects")) { effect_cmd(5, 0, 0); return 0; }
  if (IS("setEffectsVolume")) { effect_cmd(2, 0, fval(c, 0)); return 0; }
  if (IS("getEffectsVolume")) return f2u(1.0f);
  if (IS("preloadEffect")) { effect_preload(astr(c, 0)); return 0; }
  if (IS("unloadEffect") || IS("pauseEffect") || IS("resumeEffect")) return 0;
  if (IS("getClassLoader")) return jni_object("java/lang/ClassLoader");
  if (IS("loadClass") || IS("forName")) {
    char nm[256]; strncpy(nm, astr(c, 0), sizeof nm - 1); nm[255] = 0;
    for (char *p = nm; *p; p++) if (*p == '.') *p = '/';
    return jni_class(nm);
  }
  if (IS("getApplicationContext") || IS("getContext") || IS("getActivity") || IS("getInstance")) return jni_object("android/content/Context");
  static char seen[256][96]; static int nseen;
  char key[96]; snprintf(key, sizeof key, "%s.%s", k, name);
  int known = 0; for (int i = 0; i < nseen; i++) if (!strcmp(seen[i], key)) known = 1;
  if (!known && nseen < 256) { strcpy(seen[nseen++], key); LOGI("[jni] unhandled %s.%s%s -> default\n", cls, name, sig ? sig : ""); }
  if (ret == 'L' && sig && strstr(strchr(sig, ')'), "String")) return jni_str("");
  return 0;
  #undef IS
}

// ------------------------------------------------------------------ JNIEnv functions
static uint32_t g_env, g_vm, g_envtab, g_vmtab;
static void set_ret(char t, uint64_t v) {
  if (t == 'J' || t == 'D') RET64(v); else RET((uint32_t)v);
}
// slot -> (return type, mode, static?)  for the Call* families
static int call_info(int s, char *rt, int *mode, int *st, int *nonvirt) {
  static const char T[] = "LZBCSIJFDV";
  *nonvirt = 0;
  if (s >= 34 && s <= 63) { *rt = T[(s - 34) / 3]; *mode = (s - 34) % 3; *st = 0; return 1; }
  if (s >= 64 && s <= 93) { *rt = T[(s - 64) / 3]; *mode = (s - 64) % 3; *st = 0; *nonvirt = 1; return 1; }
  if (s >= 114 && s <= 143) { *rt = T[(s - 114) / 3]; *mode = (s - 114) % 3; *st = 1; return 1; }
  return 0;
}
static void jni_slot(int s) {
  char rt; int mode, st, nv;
  if (call_info(s, &rt, &mode, &st, &nv)) {
    uint32_t self = ARG(1), mid = ARG(nv ? 3 : 2);
    jmeth *m = (jmeth *)P(mid);
    uint32_t w0 = nv ? 4 : 3;
    jargs a = { mode, w0, mode ? ARG(w0) : 0 };
    jcall c; decode(m && m->kind == K_METHOD ? m->sig : NULL, &a, &c);
    uint64_t r = (m && m->kind == K_METHOD) ? java_call(m->cls, m->name, m->sig, self, &c) : 0;
    set_ret(rt, r); return;
  }
  jobj *o;
  switch (s) {
    case 4: RET(0x00010006); return;                                    // GetVersion
    case 6: RET(jni_class(gs(ARG(1)))); return;                         // FindClass
    case 10: RET(jni_class("java/lang/Object")); return;                // GetSuperclass
    case 11: RET(1); return;                                            // IsAssignableFrom
    case 13: case 14: LOGI("[jni] Throw%s %s\n", s == 14 ? "New" : "", s == 14 ? gs(ARG(2)) : ""); RET(0); return;
    case 15: RET(0); return;                                            // ExceptionOccurred
    case 16: case 17: return;                                           // ExceptionDescribe/Clear
    case 18: LOGI("[jni] FatalError: %s\n", gs(ARG(1))); rt_trap(0);
    case 19: RET(0); return;                                            // PushLocalFrame
    case 20: RET(ARG(1)); return;                                       // PopLocalFrame
    case 21: case 25: case 226: RET(ARG(1)); return;                    // NewGlobalRef / NewLocalRef / NewWeakGlobalRef
    case 22: case 23: case 227: return;                                 // Delete*Ref
    case 24: RET(ARG(1) == ARG(2)); return;                             // IsSameObject
    case 26: RET(0); return;                                            // EnsureLocalCapacity
    case 27: case 28: case 29: case 30: o = J(ARG(1)); RET(jni_object(o ? o->cls : NULL)); return;   // AllocObject / NewObject*
    case 31: o = J(ARG(1)); RET(jni_class(o ? o->cls : "java/lang/Object")); return;                  // GetObjectClass
    case 32: RET(1); return;                                            // IsInstanceOf
    case 33: case 113: case 94: case 144: {                             // Get(Static)MethodID / FieldID
      jmeth *m = calloc(1, sizeof *m); o = J(ARG(1));
      m->kind = (s == 33 || s == 113) ? K_METHOD : K_FIELD; m->cls = strdup(o ? o->cls : "?");
      m->name = strdup(gs(ARG(2))); m->sig = strdup(gs(ARG(3))); m->is_static = s == 113 || s == 144;
      RET((uint32_t)(uintptr_t)m); return; }
    case 164: o = J(ARG(1)); RET(o ? o->len : 0); return;               // GetStringLength
    case 165: { o = J(ARG(1)); uint32_t n = o ? o->len : 0; uint16_t *u = calloc(n + 1, 2);   // GetStringChars
      for (uint32_t i = 0; i < n; i++) u[i] = (uint8_t)o->str[i]; if (ARG(2)) W8(ARG(2), 1); RET((uint32_t)(uintptr_t)u); return; }
    case 166: free(ARGP(2)); return;
    case 163: { uint32_t n = ARG(2); char *b = calloc(n + 1, 1); for (uint32_t i = 0; i < n; i++) b[i] = (char)M16(ARG(1) + 2 * i); RET(jni_str(b)); free(b); return; }
    case 167: RET(jni_str(ARG(1) ? gs(ARG(1)) : "")); return;           // NewStringUTF
    case 168: o = J(ARG(1)); RET(o && o->str ? strlen(o->str) : 0); return;
    case 169: o = J(ARG(1)); if (ARG(2)) W8(ARG(2), 0); RET(o && o->str ? (uint32_t)(uintptr_t)o->str : (uint32_t)(uintptr_t)""); return;
    case 170: case 225: return;                                         // ReleaseStringUTFChars / ReleaseStringCritical
    case 171: o = J(ARG(1)); RET(o ? o->len : 0); return;               // GetArrayLength
    case 172: RET(jni_array(ARG(1), 4, "[Ljava/lang/Object;")); return; // NewObjectArray
    case 173: o = J(ARG(1)); RET(o && ARG(2) < o->len ? ((uint32_t *)o->data)[ARG(2)] : 0); return;
    case 174: o = J(ARG(1)); if (o && ARG(2) < o->len) ((uint32_t *)o->data)[ARG(2)] = ARG(3); return;
    case 219: W32(ARG(1), g_vm); RET(0); return;                        // GetJavaVM
    case 215: case 216: RET(0); return;                                 // (Un)RegisterNatives
    case 217: case 218: RET(0); return;                                 // MonitorEnter/Exit
    case 220: { o = J(ARG(1)); for (uint32_t i = 0; o && i < ARG(3); i++) W16(ARG(4) + 2 * i, (uint8_t)o->str[ARG(2) + i]); return; }
    case 221: { o = J(ARG(1)); if (o) memcpy(ARGP(4), o->str + ARG(2), ARG(3)); return; }
    case 222: o = J(ARG(1)); if (ARG(2)) W8(ARG(2), 0); RET(o ? (uint32_t)(uintptr_t)o->data : 0); return;   // GetPrimitiveArrayCritical
    case 223: return;
    case 224: { o = J(ARG(1)); uint32_t n = o ? o->len : 0; uint16_t *u = calloc(n + 1, 2); for (uint32_t i = 0; i < n; i++) u[i] = (uint8_t)o->str[i]; RET((uint32_t)(uintptr_t)u); return; }
    case 228: RET(0); return;                                           // ExceptionCheck
    case 229: { uint32_t a = mk(K_OBJECT, "java/nio/ByteBuffer"); o = J(a); o->data = ARGP(1); o->len = ARG(2); RET(a); return; }
    case 230: o = J(ARG(1)); RET(o ? (uint32_t)(uintptr_t)o->data : 0); return;
    case 231: o = J(ARG(1)); RET64(o ? o->len : 0); return;
    case 232: RET(1); return;
  }
  if (s >= 95 && s <= 112) { RET64(0); return; }                         // Get/Set<type>Field
  if (s >= 145 && s <= 162) {                                             // Get/SetStatic<type>Field
    jmeth *f = (jmeth *)P(ARG(2));
    if (s <= 153 && f && f->kind == K_FIELD) {
      if (!strcmp(f->name, "SDK_INT")) { RET(19); return; }
      if (f->sig && f->sig[0] == 'L' && strstr(f->sig, "String")) { RET(jni_str("")); return; }
    }
    RET64(0); return;
  }
  if (s >= 175 && s <= 182) {                                             // New<prim>Array
    static const uint8_t esz[] = { 1, 1, 2, 2, 4, 8, 4, 8 };
    RET(jni_array(ARG(1), esz[s - 175], "[prim")); return;
  }
  if (s >= 183 && s <= 190) { o = J(ARG(1)); if (ARG(2)) W8(ARG(2), 0); RET(o ? (uint32_t)(uintptr_t)o->data : 0); return; }
  if (s >= 191 && s <= 198) return;                                       // Release<prim>ArrayElements
  if (s >= 199 && s <= 214) {                                             // Get/Set<prim>ArrayRegion
    static const uint8_t esz[] = { 1, 1, 2, 2, 4, 8, 4, 8 };
    int set = s >= 207; uint32_t e = esz[(s - 199) % 8];
    o = J(ARG(1)); if (!o) return;
    uint32_t start = ARG(2), len = ARG(3), buf = ARG(4);
    if (set) memcpy(o->data + start * e, P(buf), len * e); else memcpy(P(buf), o->data + start * e, len * e);
    return;
  }
  LOGI("[jni] unimplemented JNIEnv slot %d\n", s); RET(0);
}
static void vm_slot(int s) {
  switch (s) {
    case 4: case 7: W32(ARG(1), g_env); RET(0); return;    // AttachCurrentThread(AsDaemon)
    case 5: RET(0); return;                                 // DetachCurrentThread
    case 6: W32(ARG(1), g_env); RET(0); return;             // GetEnv
    default: RET(0); return;
  }
}
#define S1(n) static void js_##n(void) { jni_slot(n); }
#define S10(d) S1(d##0) S1(d##1) S1(d##2) S1(d##3) S1(d##4) S1(d##5) S1(d##6) S1(d##7) S1(d##8) S1(d##9)
S1(0) S1(1) S1(2) S1(3) S1(4) S1(5) S1(6) S1(7) S1(8) S1(9)
S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9) S10(10) S10(11) S10(12) S10(13) S10(14) S10(15) S10(16) S10(17) S10(18) S10(19) S10(20) S10(21) S10(22)
S1(230) S1(231) S1(232)
#define P1(n) js_##n,
#define P10(d) P1(d##0) P1(d##1) P1(d##2) P1(d##3) P1(d##4) P1(d##5) P1(d##6) P1(d##7) P1(d##8) P1(d##9)
static void (*const jslots[233])(void) = { P1(0) P1(1) P1(2) P1(3) P1(4) P1(5) P1(6) P1(7) P1(8) P1(9)
  P10(1) P10(2) P10(3) P10(4) P10(5) P10(6) P10(7) P10(8) P10(9) P10(10) P10(11) P10(12) P10(13) P10(14) P10(15) P10(16) P10(17) P10(18) P10(19) P10(20) P10(21) P10(22)
  P1(230) P1(231) P1(232) };
static void vs3(void) { vm_slot(3); } static void vs4(void) { vm_slot(4); } static void vs5(void) { vm_slot(5); }
static void vs6(void) { vm_slot(6); } static void vs7(void) { vm_slot(7); }

uint32_t jni_env(void) {
  if (g_env) return g_env;
  g_envtab = (uint32_t)(uintptr_t)calloc(233, 4);
  for (int i = 0; i < 233; i++) W32(g_envtab + 4 * i, rt_host_fn(jslots[i]));
  g_env = (uint32_t)(uintptr_t)calloc(1, 16); W32(g_env, g_envtab);
  g_vmtab = (uint32_t)(uintptr_t)calloc(8, 4);
  W32(g_vmtab + 12, rt_host_fn(vs3)); W32(g_vmtab + 16, rt_host_fn(vs4)); W32(g_vmtab + 20, rt_host_fn(vs5));
  W32(g_vmtab + 24, rt_host_fn(vs6)); W32(g_vmtab + 28, rt_host_fn(vs7));
  g_vm = (uint32_t)(uintptr_t)calloc(1, 16); W32(g_vm, g_vmtab);
  return g_env;
}
uint32_t jni_vm(void) { jni_env(); return g_vm; }

// ------------------------------------------------------------------ AAssetManager on /assets
typedef struct { FILE *f; uint32_t len; char path[512]; } gasset;
void imp_AAssetManager_fromJava(void) { RET(0xA55E7000u); }
void imp_AAssetManager_open(void) {
  char p[600]; snprintf(p, sizeof p, "%s/assets/%s", rt_fs_root(), gs(ARG(1)));
  FILE *f = fopen(p, "rb");
  if (getenv("FILE_TRACE")) LOGI("AAsset open %s -> %s\n", p, f ? "ok" : "missing");
  if (!f) { RET(0); return; }
  gasset *a = calloc(1, sizeof *a); a->f = f; fseek(f, 0, SEEK_END); a->len = ftell(f); fseek(f, 0, SEEK_SET);
  strncpy(a->path, p, sizeof a->path - 1);
  RET((uint32_t)(uintptr_t)a);
}
void imp_AAsset_close(void) { gasset *a = (gasset *)P(ARG(0)); if (a) { fclose(a->f); free(a); } }
void imp_AAsset_getLength(void) { RET(((gasset *)P(ARG(0)))->len); }
void imp_AAsset_read(void) { gasset *a = (gasset *)P(ARG(0)); RET(fread(ARGP(1), 1, ARG(2), a->f)); }
void imp_AAsset_openFileDescriptor(void) {
  gasset *a = (gasset *)P(ARG(0)); int fd = open(a->path, O_RDONLY);
  if (ARG(1)) W32(ARG(1), 0); if (ARG(2)) W32(ARG(2), a->len); RET(fd);
}
typedef struct { DIR *d; } gdir;
void imp_AAssetManager_openDir(void) {
  char p[600]; snprintf(p, sizeof p, "%s/assets/%s", rt_fs_root(), gs(ARG(1)));
  gdir *g = calloc(1, sizeof *g); g->d = opendir(p); RET((uint32_t)(uintptr_t)g);
}
void imp_AAssetDir_getNextFileName(void) {
  gdir *g = (gdir *)P(ARG(0)); struct dirent *e;
  while (g && g->d && (e = readdir(g->d))) {
    if (e->d_type == DT_DIR) continue;
    static _Thread_local char name[256]; strncpy(name, e->d_name, 255); RET((uint32_t)(uintptr_t)name); return;
  }
  RET(0);
}
void imp_AAssetDir_close(void) { gdir *g = (gdir *)P(ARG(0)); if (g) { if (g->d) closedir(g->d); free(g); } }

// ------------------------------------------------------------------ OpenSL ES: not available yet (games fall back to silence)
void imp_slCreateEngine(void) { if (ARG(0)) W32(ARG(0), 0); RET(12); }   // SL_RESULT_FEATURE_UNSUPPORTED

// ------------------------------------------------------------------ GLES extras
static uint32_t mapped_buf; static void *mapped_ptr; static uint32_t mapped_len;
static uint32_t buf_size[65536];
void imp_glBufferData(void) {
  uint32_t target = ARG(0), size = ARG(1);
  GLint b = 0; glGetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &b);
  if (b >= 0 && b < 65536) buf_size[b] = size;
  glBufferData(target, size, ARGP(2), ARG(3));
}
void imp_glMapBufferOES(void) {
  uint32_t target = ARG(0);
  GLint b = 0; glGetIntegerv(target == GL_ELEMENT_ARRAY_BUFFER ? GL_ELEMENT_ARRAY_BUFFER_BINDING : GL_ARRAY_BUFFER_BINDING, &b);
  mapped_buf = target; mapped_len = (b >= 0 && b < 65536) ? buf_size[b] : 0;
  free(mapped_ptr); mapped_ptr = calloc(1, mapped_len ? mapped_len : 16);
  RET((uint32_t)(uintptr_t)mapped_ptr);
}
void imp_glUnmapBufferOES(void) {
  if (mapped_ptr && mapped_len) glBufferSubData(mapped_buf, 0, mapped_len, mapped_ptr);
  RET(1);
}
static void h_genvao(void) { glGenVertexArraysOES(ARG(0), ARGP(1)); }
static void h_bindvao(void) { glBindVertexArrayOES(ARG(0)); }
static void h_delvao(void) { glDeleteVertexArraysOES(ARG(0), ARGP(1)); }
static void h_isvao(void) { RET(glIsVertexArrayOES(ARG(0))); }
static void h_map(void) { imp_glMapBufferOES(); }
static void h_unmap(void) { imp_glUnmapBufferOES(); }
static void h_discard(void) {}
void imp_eglGetProcAddress(void) {
  const char *n = gs(ARG(0)); static uint32_t t[7];
  if (!t[0]) { t[0] = rt_host_fn(h_genvao); t[1] = rt_host_fn(h_bindvao); t[2] = rt_host_fn(h_delvao); t[3] = rt_host_fn(h_isvao);
               t[4] = rt_host_fn(h_map); t[5] = rt_host_fn(h_unmap); t[6] = rt_host_fn(h_discard); }
  uint32_t r = 0;
  if (!strcmp(n, "glGenVertexArraysOES")) r = t[0]; else if (!strcmp(n, "glBindVertexArrayOES")) r = t[1];
  else if (!strcmp(n, "glDeleteVertexArraysOES")) r = t[2]; else if (!strcmp(n, "glIsVertexArrayOES")) r = t[3];
  else if (!strcmp(n, "glMapBufferOES")) r = t[4]; else if (!strcmp(n, "glUnmapBufferOES")) r = t[5];
  else if (!strcmp(n, "glDiscardFramebufferEXT")) r = t[6];
  if (!r) LOGI("[egl] eglGetProcAddress(%s) -> null\n", n);
  RET(r);
}

// glTexImage2D: WebGL 1 requires internalformat == format (GLES2 drivers are lenient), so use the format.
// Uploads that still fail are logged once with their parameters.
void imp_glTexImage2D(void) {
  uint32_t target = ARGW(0), level = ARGW(1), ifmt = ARGW(2), w = ARGW(3), h = ARGW(4), border = ARGW(5), fmt = ARGW(6), type = ARGW(7), px = ARGW(8);
  if (ifmt != fmt) ifmt = fmt;
  while (glGetError() != GL_NO_ERROR) {}
  glTexImage2D(target, level, ifmt, w, h, border, fmt, type, (const void *)P(px));
  GLenum e = glGetError();
  static int logged;
  if (e != GL_NO_ERROR && logged++ < 20)
    LOGI("[gl] glTexImage2D(target=%x level=%u ifmt=%x %ux%u fmt=%x type=%x) -> error %x\n", target, level, ARGW(2), w, h, fmt, type, e);
}
