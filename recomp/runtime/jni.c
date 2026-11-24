// Fake Java VM for the recompiled game: object handles, strings, classes and method IDs.
// Java methods the game calls are logged ("JAVA ...") and answered from java_call() below.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
enum { K_FREE, K_CLASS, K_STRING, K_OBJECT, K_ARRAY };
typedef struct { int kind; char *name; char *str; uint8_t *data; uint32_t len; int elem; } jobj;
#define MAX_OBJ 65536
static jobj objs[MAX_OBJ];
static uint32_t next_obj = 1;
typedef struct { char *cls, *name, *sig; int is_static; } jmeth;
#define MAX_METH 4096
static jmeth meths[MAX_METH];
static uint32_t nmeth = 1;
static int jni_trace = 0;

uint32_t jvm, jenv;
#define H(x) (0x40000000u | (x))      // handles look like pointers no guest data will have
#define IDX(h) ((h) & 0x3FFFFFFFu)
static jobj *O(uint32_t h) { return (h & 0x40000000u) && IDX(h) < next_obj ? &objs[IDX(h)] : NULL; }

#include <pthread.h>
static pthread_mutex_t obj_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t new_obj(int kind, const char *name) {
  pthread_mutex_lock(&obj_lock);
  uint32_t i = next_obj++;
  pthread_mutex_unlock(&obj_lock); if (i >= MAX_OBJ) { LOGI("JNI: out of handles\n"); rt_trap(0); }
  objs[i].kind = kind; objs[i].name = name ? strdup(name) : NULL; return H(i);
}
uint32_t jni_new_string(const char *s) { uint32_t h = new_obj(K_STRING, "java/lang/String"); O(h)->str = strdup(s); return h; }
const char *jni_string(uint32_t h) { jobj *o = O(h); return o && o->kind == K_STRING ? o->str : NULL; }
uint32_t jni_new_object(const char *cls) { return new_obj(K_OBJECT, cls); }

static uint32_t find_class(const char *name) {
  for (uint32_t i = 1; i < next_obj; i++)
    if (objs[i].kind == K_CLASS && !strcmp(objs[i].name, name)) return H(i);
  return new_obj(K_CLASS, name);
}
static pthread_mutex_t meth_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t get_method_locked(uint32_t cls, const char *name, const char *sig, int st);
static uint32_t get_method(uint32_t cls, const char *name, const char *sig, int st) {
  pthread_mutex_lock(&meth_lock); uint32_t r = get_method_locked(cls, name, sig, st); pthread_mutex_unlock(&meth_lock); return r;
}
static uint32_t get_method_locked(uint32_t cls, const char *name, const char *sig, int st) {
  jobj *c = O(cls); const char *cn = c && c->name ? c->name : "?";
  for (uint32_t i = 1; i < nmeth; i++)
    if (!strcmp(meths[i].cls, cn) && !strcmp(meths[i].name, name) && !strcmp(meths[i].sig, sig)) return i;
  meths[nmeth] = (jmeth){strdup(cn), strdup(name), strdup(sig), st};
  return nmeth++;
}

// ---------------------------------------------------------------- argument decoding
// Returns arguments as uint64 per Java param according to the signature.
// mode 0: varargs on guest stack at `p` (float promoted to double), 1: va_list pointer, 2: jvalue[]
static int decode_args(const char *sig, uint32_t p, int mode, uint64_t *out, char *types) {
  int n = 0; const char *s = sig + 1;
  while (*s && *s != ')') {
    char t = *s;
    if (t == 'L') { while (*s != ';') s++; }
    else if (t == '[') { while (*s == '[') s++; if (*s == 'L') while (*s != ';') s++; t = 'L'; }
    s++;
    if (mode == 2) { out[n] = M64(p); p += 8; }
    else if (t == 'J' || t == 'D' || (t == 'F' && mode != 2)) { out[n] = M64(p); p += 8; }
    else { out[n] = M32(p); p += 4; }
    types[n++] = t;
  }
  return n;
}
static const char *ret_type(const char *sig) { const char *r = strchr(sig, ')'); return r ? r + 1 : "V"; }

// ---------------------------------------------------------------- Java behaviour
// Answers for the Java methods the engine asks about. Unknown ones return 0/null and are logged.
static int seen_calls;
// SharedPreferences stand-in: key\tvalue lines in <FS_ROOT>/prefs.txt
#define MAX_KV 512
static char *kv_k[MAX_KV], *kv_v[MAX_KV]; static int kv_n, kv_loaded;
static char kv_path[600];
static void kv_load(void) {
  if (kv_loaded) return; kv_loaded = 1;
  snprintf(kv_path, sizeof kv_path, "%s/prefs.txt", getenv("FS_ROOT") ? getenv("FS_ROOT") : "/fs");   // same default as hle.c fs_root
  FILE *f = fopen(kv_path, "r"); if (!f) return;
  char line[8192];
  while (kv_n < MAX_KV && fgets(line, sizeof line, f)) {
    char *t = strchr(line, '\t'); if (!t) continue; *t = 0; line[strcspn(line, "\n")] = 0; t[1 + strcspn(t + 1, "\n")] = 0;
    kv_k[kv_n] = strdup(line); kv_v[kv_n++] = strdup(t + 1);
  }
  fclose(f);
}
static void kv_save(void) {
  FILE *f = fopen(kv_path, "w"); if (!f) return;
  for (int i = 0; i < kv_n; i++) if (kv_k[i]) fprintf(f, "%s\t%s\n", kv_k[i], kv_v[i]);
  fclose(f);
}
static int kv_find(const char *k) { kv_load(); for (int i = 0; i < kv_n; i++) if (kv_k[i] && !strcmp(kv_k[i], k)) return i; return -1; }
// UI requests from the game, consumed by the browser frame loop (hle.c, WEB_BUILD).
int ui_kb_req, ui_kb_max = 64, ui_kb_native, ui_kb_hide; char ui_kb_text[512];
// TitanWebView (News Royale): the page drives an iframe from these (hle.c web_input).
int ui_wv_dirty, ui_wv_x, ui_wv_y, ui_wv_w, ui_wv_h, ui_wv_loaded; char ui_wv_url[1024];
int ui_dlg_id, ui_dlg_pending; char ui_dlg[5][1024];

static uint64_t java_call(jmeth *m, uint32_t self, uint64_t *a, char *t, int n) {
  const char *nm = m->name;
  const char *rt = ret_type(m->sig);
  int quiet = !jni_trace && (!strcmp(nm, "existsKeyValue") || !strcmp(nm, "getKeyValue") || !strcmp(nm, "storeKeyValue"));
  if (!quiet && (jni_trace || seen_calls < 400 || strstr(m->cls, "WebView"))) {
    LOGI("JAVA %s%s.%s%s(", m->is_static ? "static " : "", m->cls, nm, m->sig);
    for (int i = 0; i < n; i++) {
      if (t[i] == 'L' && jni_string((uint32_t)a[i])) LOGI("%s\"%s\"", i ? ", " : "", jni_string((uint32_t)a[i]));
      else LOGI("%s%llx", i ? ", " : "", (unsigned long long)a[i]);
    }
    LOGI(")\n");
    seen_calls++;
  }
#define IS(x) (!strcmp(nm, x))
  const char *s0 = n > 0 && t[0] == 'L' ? jni_string((uint32_t)a[0]) : NULL;
  const char *s1 = n > 1 && t[1] == 'L' ? jni_string((uint32_t)a[1]) : NULL;
  if (IS("existsKeyValue") && s0) return kv_find(s0) >= 0;
  if (IS("getKeyValue") && s0) { int i = kv_find(s0); return jni_new_string(i >= 0 ? kv_v[i] : ""); }
  if (IS("storeKeyValue") && s0) {
    int i = kv_find(s0);
    if (i < 0 && kv_n < MAX_KV) { i = kv_n++; kv_k[i] = strdup(s0); } else if (i >= 0) free(kv_v[i]);
    if (i >= 0) kv_v[i] = strdup(s1 ? s1 : ""); kv_save(); return 0;
  }
  // showKeyboard(native, hint): native=true means Android shows its own text box above the keyboard
  // (clan chat); false means the game draws the field itself (name entry).
  if (IS("showKeyboard")) { ui_kb_req = 1; ui_kb_hide = 0; ui_kb_native = n > 0 && a[0]; return 0; }
  if (IS("hideKeyboard")) { ui_kb_req = 0; ui_kb_hide = 1; return 0; }
  if (IS("setText") && n > 0) {   // setText([B): UTF-8 bytes
    if (s0) snprintf(ui_kb_text, sizeof ui_kb_text, "%s", s0);
    else { jobj *o = O((uint32_t)a[0]); uint32_t l = o && o->data ? o->len : 0; if (l >= sizeof ui_kb_text) l = sizeof ui_kb_text - 1;
           if (o && o->data) memcpy(ui_kb_text, o->data, l); ui_kb_text[l] = 0; }
    return 0;
  }
  if (IS("loadURL") && s0) { snprintf(ui_wv_url, sizeof ui_wv_url, "%s", s0); ui_wv_dirty = 1; ui_wv_loaded = 1; return 0; }
  if ((IS("hide") || IS("close")) && strstr(m->cls, "WebView")) { ui_wv_w = ui_wv_h = 0; ui_wv_dirty = 1; return 0; }
  if (IS("loadHTML")) { snprintf(ui_wv_url, sizeof ui_wv_url, "about:blank"); ui_wv_dirty = 1; ui_wv_loaded = 1; return 0; }
  if (IS("show") && n >= 4 && !strcmp(m->sig, "(IIII)V")) { ui_wv_x = (int)a[0]; ui_wv_y = (int)a[1]; ui_wv_w = (int)a[2]; ui_wv_h = (int)a[3]; ui_wv_dirty = 1; return 0; }
  if (IS("setMaxTextLength") && n > 0) { ui_kb_max = (int)a[0]; return 0; }
  if (IS("ShowDialog")) {
    for (int i = 0; i < 5; i++) { const char *v = i < n && t[i] == 'L' ? jni_string((uint32_t)a[i]) : NULL; snprintf(ui_dlg[i], sizeof ui_dlg[i], "%s", v ? v : ""); }
    ui_dlg_pending = ++ui_dlg_id;
    if (!strncmp(ui_dlg[0], "Important - Please read", 23)) ui_dlg_pending = -ui_dlg_id;   // first-run terms: auto-accept
    return ui_dlg_id;
  }
  if (IS("removeKeyValue") && s0) { int i = kv_find(s0); if (i >= 0) { free(kv_k[i]); kv_k[i] = NULL; kv_save(); } return 0; }
  if (*rt == 'L' && !strncmp(rt, "Ljava/lang/String;", 18)) {
    if (IS("getDeviceModel") || IS("getModel")) return jni_new_string("WebBrowser");
    if (IS("getOSVersion") || IS("getSystemVersion")) return jni_new_string("7.0");
    if (IS("getLanguage")) return jni_new_string("en");
    if (IS("getPreferredLanguage")) return jni_new_string("en-US");   // Android-style tag; servers slice it
    if (IS("getCountry") || IS("getCountryCode")) return jni_new_string("US");
    if (IS("getAppVersion") || IS("getVersionName")) return jni_new_string("1.9.2");
    return jni_new_string("");
  }
  if (*rt == 'L' || *rt == '[') return new_obj(K_OBJECT, rt);
  if (IS("getTotalMemory")) return 512;   // low-memory device: the game then sticks to the lowres textures we ship (hires ones come from a dead CDN)
  if (*rt == 'Z') {
    if (IS("isSignatureValid") || IS("isNativeLibraryLoaded") || IS("isNetworkAvailable") || IS("hasInternetConnection")) return 1;
    return 0;
  }
  return 0;
#undef IS
}

static void call_common(int mode, int st, char kind) {
  // stack: env, obj|cls, methodID, args...
  uint32_t self = ARG(1), mid = ARG(2);
  if (!mid || mid >= nmeth) { LOGI("JNI: bad methodID %x\n", mid); g_eax = 0; return; }
  uint64_t a[32]; char t[32];
  uint32_t p = mode == 0 ? g_esp + 16 : ARG(3);
  int n = decode_args(meths[mid].sig, p, mode, a, t);
  uint64_t r = java_call(&meths[mid], self, a, t, n);
  if (kind == 'F' || kind == 'D') { double d; memcpy(&d, &r, 8); fpush(r ? d : 0.0); return; }
  g_eax = (uint32_t)r; g_edx = (uint32_t)(r >> 32);
}

// ---------------------------------------------------------------- the JNIEnv function table
static void env_GetVersion(void) { g_eax = 0x10006; }
static void env_FindClass(void) {
  g_eax = find_class((const char *)P(ARG(1)));
  if (jni_trace) LOGI("JNI FindClass %s\n", (const char *)P(ARG(1)));
}
static void env_GetObjectClass(void) { jobj *o = O(ARG(1)); g_eax = find_class(o && o->name ? o->name : "java/lang/Object"); }
static void env_GetMethodID(void) { g_eax = get_method(ARG(1), (char *)P(ARG(2)), (char *)P(ARG(3)), 0); }
static void env_GetStaticMethodID(void) { g_eax = get_method(ARG(1), (char *)P(ARG(2)), (char *)P(ARG(3)), 1); }
static void env_GetFieldID(void) { g_eax = get_method(ARG(1), (char *)P(ARG(2)), (char *)P(ARG(3)), 0); }
static void env_GetStaticFieldID(void) { g_eax = get_method(ARG(1), (char *)P(ARG(2)), (char *)P(ARG(3)), 1); }
static void env_GetField(void) {
  uint32_t f = ARG(2);
  LOGI("JAVA field get %s.%s %s\n", f < nmeth ? meths[f].cls : "?", f < nmeth ? meths[f].name : "?", f < nmeth ? meths[f].sig : "");
  g_eax = 0; g_edx = 0;
}
static void env_SetField(void) { g_eax = 0; }
static void env_ref_identity(void) { g_eax = ARG(1); }
static void env_zero(void) { g_eax = 0; }
static void env_void(void) {}
static void env_IsSameObject(void) { g_eax = ARG(1) == ARG(2); }
static void env_NewStringUTF(void) { g_eax = ARG(1) ? jni_new_string((const char *)P(ARG(1))) : 0; }
static void env_GetStringUTFChars(void) {
  const char *s = jni_string(ARG(1)); if (ARG(2)) W8(ARG(2), 1);
  g_eax = (uint32_t)(uintptr_t)(s ? s : "");
}
static void env_GetStringChars(void) {
  const char *s = jni_string(ARG(1)); if (!s) s = ""; if (ARG(2)) W8(ARG(2), 1);
  size_t n = strlen(s); uint16_t *w = calloc(n + 1, 2);
  for (size_t i = 0; i < n; i++) w[i] = (uint8_t)s[i];
  g_eax = (uint32_t)(uintptr_t)w;
}
static void env_ReleaseStringChars(void) { free((void *)P(ARG(2))); }
static void env_NewString(void) {   // (env, const jchar*, len)
  uint32_t n = ARG(2); char *b = malloc(n + 1);
  for (uint32_t i = 0; i < n; i++) { uint16_t c = M16(ARG(1) + 2 * i); b[i] = c < 128 ? (char)c : '?'; }
  b[n] = 0; g_eax = jni_new_string(b); free(b);
}
static void env_GetStringRegion(void) {
  const char *s = jni_string(ARG(1)); if (!s) s = "";
  for (uint32_t i = 0; i < ARG(3); i++) W16(ARG(4) + 2 * i, (uint8_t)s[ARG(2) + i]);
}
static void env_GetStringUTFLength(void) { const char *s = jni_string(ARG(1)); g_eax = s ? strlen(s) : 0; }
static void env_GetStringLength(void) { const char *s = jni_string(ARG(1)); g_eax = s ? strlen(s) : 0; }
static void env_GetStringUTFRegion(void) {
  const char *s = jni_string(ARG(1)); if (!s) s = "";
  memcpy((void *)P(ARG(4)), s + ARG(2), ARG(3)); W8(ARG(4) + ARG(3), 0);
}
static void env_NewObject(void) {
  jobj *c = O(ARG(1)); uint32_t mid = ARG(2);
  uint32_t h = new_obj(K_OBJECT, c && c->name ? c->name : "?");
  if (mid < nmeth) { uint64_t a[32]; char t[32]; int n = decode_args(meths[mid].sig, g_esp + 16, 0, a, t);
                     java_call(&meths[mid], h, a, t, n); }
  g_eax = h;
}
static uint32_t new_array(int elem, uint32_t len) {
  uint32_t h = new_obj(K_ARRAY, "[");
  jobj *o = O(h); o->elem = elem; o->len = len; o->data = calloc(len ? len : 1, elem);
  return h;
}
static void env_GetArrayLength(void) { jobj *o = O(ARG(1)); g_eax = o ? o->len : 0; }
static void env_NewByteArray(void) { g_eax = new_array(1, ARG(1)); }
static void env_NewIntArray(void) { g_eax = new_array(4, ARG(1)); }
static void env_NewObjectArray(void) { g_eax = new_array(4, ARG(1)); }
static void env_GetObjectArrayElement(void) { jobj *o = O(ARG(1)); g_eax = o && ARG(2) < o->len ? ((uint32_t *)o->data)[ARG(2)] : 0; }
static void env_SetObjectArrayElement(void) { jobj *o = O(ARG(1)); if (o && ARG(2) < o->len) ((uint32_t *)o->data)[ARG(2)] = ARG(3); }
static void env_GetArrayElements(void) { jobj *o = O(ARG(1)); if (ARG(2)) W8(ARG(2), 0); g_eax = o ? (uint32_t)(uintptr_t)o->data : 0; }
static void env_GetArrayRegion(void) { jobj *o = O(ARG(1)); if (o) memcpy((void *)P(ARG(4)), o->data + ARG(2) * o->elem, ARG(3) * o->elem); }
static void env_SetArrayRegion(void) { jobj *o = O(ARG(1)); if (o) memcpy(o->data + ARG(2) * o->elem, (void *)P(ARG(4)), ARG(3) * o->elem); }
static void env_RegisterNatives(void) {
  jobj *c = O(ARG(1)); uint32_t tab = ARG(2), n = ARG(3);
  for (uint32_t i = 0; i < n; i++)
    LOGI("RegisterNatives %s.%s%s -> %x\n", c ? c->name : "?", (char *)P(M32(tab + 12 * i)), (char *)P(M32(tab + 12 * i + 4)), M32(tab + 12 * i + 8) - B);
  g_eax = 0;
}
static void env_GetJavaVM(void) { W32(ARG(1), jvm); g_eax = 0; }
static void env_ExceptionCheck(void) { g_eax = 0; }
static void env_GetDirectBufferAddress(void) { g_eax = 0; }

#define CALLS(base, st) \
  static void c##base##_0(void){call_common(0,st,'L');} static void c##base##_1(void){call_common(1,st,'L');} static void c##base##_2(void){call_common(2,st,'L');}
// One handler triple per return kind; float/double need the x87 path.
#define CK(name, kind, st) static void name##_0(void){call_common(0,st,kind);} static void name##_1(void){call_common(1,st,kind);} static void name##_2(void){call_common(2,st,kind);}
CK(ci, 'I', 0) CK(cf, 'F', 0) CK(cd, 'D', 0) CK(si, 'I', 1) CK(sf, 'F', 1) CK(sd, 'D', 1)

static void (*env_fn(int slot))(void) {
  switch (slot) {
    case 4: return env_GetVersion; case 6: return env_FindClass;
    case 10: return env_zero; case 11: return env_zero; case 13: case 14: return env_zero;
    case 15: return env_zero; case 16: case 17: return env_void;
    case 19: return env_zero; case 20: return env_ref_identity;
    case 21: case 25: case 226: return env_ref_identity; case 22: case 23: case 227: return env_void;
    case 24: return env_IsSameObject; case 26: return env_zero;
    case 28: case 29: case 30: return env_NewObject; case 27: return env_NewObject;
    case 31: return env_GetObjectClass; case 32: return env_zero; case 33: return env_GetMethodID;
    case 94: return env_GetFieldID; case 113: return env_GetStaticMethodID; case 144: return env_GetStaticFieldID;
    case 163: return env_NewString; case 164: return env_GetStringLength;
    case 165: case 224: return env_GetStringChars; case 166: case 225: return env_ReleaseStringChars;
    case 220: return env_GetStringRegion;
    case 167: return env_NewStringUTF; case 168: return env_GetStringUTFLength;
    case 169: return env_GetStringUTFChars; case 170: return env_void;
    case 171: return env_GetArrayLength; case 172: return env_NewObjectArray;
    case 173: return env_GetObjectArrayElement; case 174: return env_SetObjectArrayElement;
    case 176: return env_NewByteArray; case 175: return env_NewByteArray; case 179: return env_NewIntArray;
    case 215: return env_RegisterNatives; case 216: return env_zero;
    case 217: case 218: return env_zero; case 219: return env_GetJavaVM;
    case 221: return env_GetStringUTFRegion; case 228: return env_ExceptionCheck;
    case 230: return env_GetDirectBufferAddress; case 231: return env_zero; case 232: return env_zero;
  }
  if (slot >= 183 && slot <= 190) return env_GetArrayElements;
  if (slot >= 191 && slot <= 198) return env_void;
  if (slot >= 199 && slot <= 206) return env_GetArrayRegion;
  if (slot >= 207 && slot <= 214) return env_SetArrayRegion;
  if (slot == 222) return env_GetArrayElements; if (slot == 223) return env_void;
  if (slot >= 95 && slot <= 103) return env_GetField; if (slot >= 104 && slot <= 112) return env_SetField;
  if (slot >= 145 && slot <= 153) return env_GetField; if (slot >= 154 && slot <= 162) return env_SetField;
  // Call<Type>Method{,V,A}: 34..63 instance, 64..93 nonvirtual (has extra jclass arg — rare), 114..143 static
  if (slot >= 34 && slot <= 63) {
    int k = (slot - 34) / 3, v = (slot - 34) % 3;
    if (k == 7) return v == 0 ? cf_0 : v == 1 ? cf_1 : cf_2;
    if (k == 8) return v == 0 ? cd_0 : v == 1 ? cd_1 : cd_2;
    return v == 0 ? ci_0 : v == 1 ? ci_1 : ci_2;
  }
  if (slot >= 114 && slot <= 143) {
    int k = (slot - 114) / 3, v = (slot - 114) % 3;
    if (k == 7) return v == 0 ? sf_0 : v == 1 ? sf_1 : sf_2;
    if (k == 8) return v == 0 ? sd_0 : v == 1 ? sd_1 : sd_2;
    return v == 0 ? si_0 : v == 1 ? si_1 : si_2;
  }
  return NULL;
}

extern void (*jni_default_slot(int slot))(void);
static void vm_GetEnv(void) { W32(ARG(1), jenv); g_eax = 0; }
static void vm_Attach(void) { W32(ARG(1), jenv); g_eax = 0; }
static void vm_Detach(void) { g_eax = 0; }

void jni_init(void) {
  jni_trace = getenv("JNI_TRACE") != NULL;
  uint32_t tab = (uint32_t)(uintptr_t)calloc(233, 4);
  for (int i = 0; i < 233; i++) {
    void (*f)(void) = env_fn(i);
    W32(tab + 4 * i, rt_host_fn(f ? f : jni_default_slot(i)));
  }
  jenv = (uint32_t)(uintptr_t)calloc(1, 4); W32(jenv, tab);
  uint32_t vt = (uint32_t)(uintptr_t)calloc(8, 4);
  W32(vt + 16, rt_host_fn(vm_Attach)); W32(vt + 20, rt_host_fn(vm_Detach));
  W32(vt + 24, rt_host_fn(vm_GetEnv)); W32(vt + 28, rt_host_fn(vm_Attach));
  jvm = (uint32_t)(uintptr_t)calloc(1, 4); W32(jvm, vt);
}
