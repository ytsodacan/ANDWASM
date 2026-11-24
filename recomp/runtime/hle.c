// Hand-written Android/bionic HLE + boot sequence.
// Anything ABI-sensitive (structs, varargs, callbacks into guest code, data symbols) lives here;
// plain 1:1 wrappers are generated into hle_gen.c by gen_hle.py.
#include "rt.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/select.h>
#include <emscripten.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)

// ---------------------------------------------------------------- guest memory helpers
static uint32_t galloc(uint32_t n) { void *p = calloc(1, n); return (uint32_t)(uintptr_t)p; }
static const char *gstr(uint32_t a) { return (const char *)P(a); }

// ---------------------------------------------------------------- data imports
// bionic exposes these as variables; GLOB_DAT relocations want the variable's address.
static uint32_t d_stack_guard, d_sF, d_ctype, d_tolower, d_toupper, d_timezone, d_tzname;

static void init_data(void) {
  d_stack_guard = galloc(4); W32(d_stack_guard, 0x2B1D5EEDu);
  d_sF = galloc(3 * 84);                            // fake FILE structs: stdin, stdout, stderr
  // _ctype_: pointer to a 257-entry table (index -1..255); bionic bit layout
  uint32_t ct = galloc(257);
  for (int c = 0; c < 256; c++) {
    uint8_t v = 0;
    if (c < 128) {
      if (isupper(c)) v |= 0x01; if (islower(c)) v |= 0x02; if (isdigit(c)) v |= 0x04;
      if (isspace(c)) v |= 0x08; if (ispunct(c)) v |= 0x10; if (iscntrl(c)) v |= 0x20;
      if (isxdigit(c)) v |= 0x40; if (c == ' ') v |= 0x80;
    }
    W8(ct + 1 + c, v);
  }
  d_ctype = galloc(4); W32(d_ctype, ct);
  uint32_t lo = galloc(257 * 2), up = galloc(257 * 2);
  W16(lo, 0xFFFF); W16(up, 0xFFFF);
  for (int c = 0; c < 256; c++) { W16(lo + 2 + 2 * c, tolower(c)); W16(up + 2 + 2 * c, toupper(c)); }
  d_tolower = galloc(4); W32(d_tolower, lo);
  d_toupper = galloc(4); W32(d_toupper, up);
  d_timezone = galloc(4);
  uint32_t utc = galloc(4); memcpy((void *)P(utc), "UTC", 4);
  d_tzname = galloc(8); W32(d_tzname, utc); W32(d_tzname + 4, utc);
}

uint32_t hle_data_import(const char *n) {
  if (!d_sF) init_data();
  if (!strcmp(n, "__stack_chk_guard")) return d_stack_guard;
  if (!strcmp(n, "__sF")) return d_sF;
  if (!strcmp(n, "_ctype_")) return d_ctype;
  if (!strcmp(n, "_tolower_tab_")) return d_tolower;
  if (!strcmp(n, "_toupper_tab_")) return d_toupper;
  if (!strcmp(n, "timezone")) return d_timezone;
  if (!strcmp(n, "tzname")) return d_tzname;
  return 0;
}

// ---------------------------------------------------------------- filesystem (paths sandboxed under FS_ROOT)
static char fs_root[512] = "/fs";
static const char *asset_root = "/assets";
static const char *gpath(uint32_t a, char *buf) {
  const char *p = gstr(a);
  if (p[0] == '/') snprintf(buf, 1024, "%s%s", fs_root, p); else snprintf(buf, 1024, "%s/%s", asset_root, p);
  return buf;
}
static int linux_errno(int e) {   // host (Emscripten/WASI) errno -> Linux/bionic numbering
  switch (e) {
    case 0: return 0; case EPERM: return 1; case ENOENT: return 2; case EINTR: return 4; case EIO: return 5;
    case EBADF: return 9; case EAGAIN: return 11; case ENOMEM: return 12; case EACCES: return 13; case EEXIST: return 17;
    case ENOTDIR: return 20; case EISDIR: return 21; case EINVAL: return 22; case ENOSPC: return 28; case EPIPE: return 32;
    case ENOTEMPTY: return 39; case ENOTSOCK: return 88; case EADDRINUSE: return 98; case ENETUNREACH: return 101;
    case ECONNRESET: return 104; case ENOTCONN: return 107; case ETIMEDOUT: return 110; case ECONNREFUSED: return 111;
    case EALREADY: return 114; case EINPROGRESS: return 115; default: return 5;
  }
}
_Thread_local int g_errno;
// ---------------------------------------------------------------- stdio (guest FILE* = host FILE*, except __sF)
static FILE *hf(uint32_t f) {
  if (f == d_sF) return stdin;
  if (f == d_sF + 84) return stdout;
  if (f == d_sF + 168) return stderr;
  return *(FILE **)P(f + 84);   // bionic-layout FILE shell (see wrap_file)
}
// The game reads bionic FILE fields directly (e.g. fstat(fp->_file) at +0xe), so guest FILE* is a
// 96-byte bionic-shaped shell: _file at +0xe, host FILE* stashed at +84.
static uint32_t wrap_file(FILE *f) {
  if (!f) return 0;
  uint8_t *g = calloc(1, 96);
  *(int16_t *)(g + 0xe) = (int16_t)fileno(f);
  *(FILE **)(g + 84) = f;
  return (uint32_t)(uintptr_t)g;
}
void imp_fopen(void) {
  char b[1024]; const char *p = gpath(ARG(0), b);
  FILE *f = fopen(p, gstr(ARG(1)));
  if (getenv("FILE_TRACE")) LOGI("fopen(%s, %s) -> %p\n", p, gstr(ARG(1)), (void *)f);
  if (!f) g_errno = linux_errno(errno);
  if (getenv("FILE_TRACE") && strstr(p, getenv("FILE_TRACE"))) {
    LOGI("  callers:");
    for (int i = 0; i < 128; i++) { uint32_t v = M32(g_esp + 4 * i); if (v >= B && v < B + IMAGE_SIZE) LOGI(" %x", v - B); }
    LOGI("\n");
  }
  g_eax = wrap_file(f);
}
void imp_fclose(void) {
  uint32_t g = ARG(0); g_eax = fclose(hf(g));
  if (g != d_sF && g != d_sF + 84 && g != d_sF + 168) free((void *)P(g));
}
void imp_fdopen(void) { g_eax = wrap_file(fdopen(ARG(0), gstr(ARG(1)))); }
void imp_fread(void) { g_eax = fread(ARGP(0), ARG(1), ARG(2), hf(ARG(3))); }
void imp_fwrite(void) { g_eax = fwrite(ARGP(0), ARG(1), ARG(2), hf(ARG(3))); }
void imp_fseek(void) { g_eax = fseek(hf(ARG(0)), (int32_t)ARG(1), ARG(2)); }
void imp_fseeko(void) { g_eax = fseeko(hf(ARG(0)), (int32_t)ARG(1), ARG(2)); }
void imp_ftell(void) { if (getenv("FILE_TRACE")) LOGI("ftell(%x)\n", ARG(0)); g_eax = (uint32_t)ftell(hf(ARG(0))); }
void imp_ftello(void) { g_eax = (uint32_t)ftello(hf(ARG(0))); }
void imp_fflush(void) { g_eax = fflush(ARG(0) ? hf(ARG(0)) : NULL); }
void imp_fgetc(void) { g_eax = fgetc(hf(ARG(0))); }
void imp_fgets(void) { g_eax = (uint32_t)(uintptr_t)fgets(ARGP(0), ARG(1), hf(ARG(2))); }
void imp_fputc(void) { g_eax = fputc(ARG(0), hf(ARG(1))); }
void imp_fputs(void) { g_eax = fputs(gstr(ARG(0)), hf(ARG(1))); }
void imp_fputwc(void) { g_eax = fputc(ARG(0), hf(ARG(1))); }
void imp_rewind(void) { rewind(hf(ARG(0))); }

// ---------------------------------------------------------------- printf family (i386 va_list = pointer into stack)
// Re-implemented formatter: walks the format string and pulls args from guest memory.
static int gvformat(char *out, size_t cap, const char *fmt, uint32_t ap) {
  size_t n = 0;
  char spec[32], tmp[512];
  while (*fmt) {
    if (*fmt != '%') { if (n + 1 < cap) out[n] = *fmt; n++; fmt++; continue; }
    const char *s = fmt++;
    while (*fmt && strchr("-+ #0123456789.*", *fmt)) {
      if (*fmt == '*') { /* width from args */ }
      fmt++;
    }
    int lng = 0;
    while (*fmt == 'l' || *fmt == 'h' || *fmt == 'z' || *fmt == 'j' || *fmt == 't' || *fmt == 'L' || *fmt == 'q') {
      if (*fmt == 'l' || *fmt == 'q' || *fmt == 'L' || *fmt == 'j') lng++;
      fmt++;
    }
    char c = *fmt ? *fmt++ : 0;
    size_t sl = fmt - s; if (sl >= sizeof spec) sl = sizeof spec - 1;
    memcpy(spec, s, sl); spec[sl] = 0;
    // strip length modifiers, then re-add what the host needs
    char clean[32]; size_t k = 0;
    for (size_t i = 0; i < sl - 1; i++) if (!strchr("lhzjtLq", spec[i])) clean[k++] = spec[i];
    int len = 0;
    if (strchr(clean, '*')) { len = snprintf(tmp, sizeof tmp, "%s", "*"); }
    switch (c) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
        if (lng >= 2) { uint64_t v = M64(ap); ap += 8; clean[k++] = 'l'; clean[k++] = 'l'; clean[k++] = c; clean[k] = 0;
                        len = snprintf(tmp, sizeof tmp, clean, v); }
        else { uint32_t v = M32(ap); ap += 4; clean[k++] = c; clean[k] = 0; len = snprintf(tmp, sizeof tmp, clean, v); }
        break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        double v = MF64(ap); ap += 8; clean[k++] = c; clean[k] = 0; len = snprintf(tmp, sizeof tmp, clean, v); break; }
      case 's': { uint32_t v = M32(ap); ap += 4; clean[k++] = 's'; clean[k] = 0;
                  len = snprintf(tmp, sizeof tmp, clean, v ? gstr(v) : "(null)"); break; }
      case 'p': { uint32_t v = M32(ap); ap += 4; len = snprintf(tmp, sizeof tmp, "0x%x", v); break; }
      case '%': len = 1; tmp[0] = '%'; tmp[1] = 0; break;
      case 'n': ap += 4; len = 0; tmp[0] = 0; break;
      default: len = 0; tmp[0] = 0;
    }
    if (len > (int)sizeof tmp - 1) len = sizeof tmp - 1;
    for (int i = 0; i < len; i++) { if (n + 1 < cap) out[n] = tmp[i]; n++; }
  }
  if (cap) out[n < cap ? n : cap - 1] = 0;
  return (int)n;
}
void imp_fprintf(void) { char b[4096]; int n = gvformat(b, sizeof b, gstr(ARG(1)), g_esp + 12);
                         fputs(b, hf(ARG(0))); g_eax = n; }
void imp_vasprintf(void) { char b[8192]; int n = gvformat(b, sizeof b, gstr(ARG(1)), ARG(2));
                           W32(ARG(0), (uint32_t)(uintptr_t)strdup(b)); g_eax = n; }
void imp___android_log_print(void) {
  char b[4096]; gvformat(b, sizeof b, gstr(ARG(2)), g_esp + 16);
  LOGI("[%s] %s\n", ARG(1) ? gstr(ARG(1)) : "", b); g_eax = 1;
}
void imp___assert2(void) {
  LOGI("assert: %s:%u %s: %s\n", gstr(ARG(0)), ARG(1), gstr(ARG(2)), gstr(ARG(3))); rt_trap(0);
}
void imp___stack_chk_fail(void) { LOGI("stack smashing detected\n"); rt_trap(0); }

// ---------------------------------------------------------------- misc libc
void imp___errno(void) { g_eax = (uint32_t)(uintptr_t)&g_errno; }
void imp___cxa_atexit(void) { g_eax = 0; }
void imp___cxa_finalize(void) {}
void imp_setlocale(void) { static char c[] = "C"; g_eax = (uint32_t)(uintptr_t)c; }
void imp___system_property_get(void) { W8(ARG(1), 0); g_eax = 0; }
void imp___fpclassifyd(void) {
  double d; memcpy(&d, (void *)P(g_esp + 4), 8);
  int c = fpclassify(d);  // bionic: FP_INFINITE=1 FP_NAN=2 FP_NORMAL=4 FP_SUBNORMAL=8 FP_ZERO=16
  g_eax = c == FP_INFINITE ? 1 : c == FP_NAN ? 2 : c == FP_NORMAL ? 4 : c == FP_SUBNORMAL ? 8 : 16;
}
void imp___isfinite(void) { double d; memcpy(&d, (void *)P(g_esp + 4), 8); g_eax = isfinite(d); }
void imp___signbit(void) { double d; memcpy(&d, (void *)P(g_esp + 4), 8); g_eax = signbit(d) != 0; }
void imp_sysconf(void) {
  switch (ARG(0)) { case 0x27: g_eax = 4096; break;          // _SC_PAGESIZE (bionic 0x27)
                    case 0x61: case 0x60: g_eax = 1; break;  // NPROCESSORS_CONF/ONLN
                    default: g_eax = (uint32_t)-1; }
}
void imp_time(void) { uint32_t t = (uint32_t)time(NULL); if (ARG(0)) W32(ARG(0), t); g_eax = t; }
void imp_gettimeofday(void) {
  struct timeval tv; gettimeofday(&tv, NULL);
  if (ARG(0)) { W32(ARG(0), (uint32_t)tv.tv_sec); W32(ARG(0) + 4, (uint32_t)tv.tv_usec); }
  g_eax = 0;
}
void imp_clock_gettime(void) {
  struct timespec ts; clock_gettime(ARG(0) == 1 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &ts);
  W32(ARG(1), (uint32_t)ts.tv_sec); W32(ARG(1) + 4, (uint32_t)ts.tv_nsec); g_eax = 0;
}
void imp_nanosleep(void) { uint32_t ts = ARG(0); usleep(M32(ts) * 1000000u + M32(ts + 4) / 1000u); g_eax = 0; }
void imp_tzset(void) {}
static void tm_out(uint32_t a, struct tm *t) {
  int v[11] = {t->tm_sec, t->tm_min, t->tm_hour, t->tm_mday, t->tm_mon, t->tm_year, t->tm_wday, t->tm_yday, t->tm_isdst, (int)t->tm_gmtoff, 0};
  for (int i = 0; i < 10; i++) W32(a + 4 * i, v[i]);
  W32(a + 40, 0);
}
void imp_localtime_r(void) { time_t t = (int32_t)M32(ARG(0)); struct tm r; localtime_r(&t, &r); tm_out(ARG(1), &r); g_eax = ARG(1); }
void imp_gmtime(void) { static uint32_t buf; if (!buf) buf = galloc(44); time_t t = (int32_t)M32(ARG(0)); struct tm r; gmtime_r(&t, &r); tm_out(buf, &r); g_eax = buf; }
void imp_qsort(void) {
  // insertion sort calling the guest comparator (n is small in practice)
  uint32_t base = ARG(0), n = ARG(1), sz = ARG(2), cmp = ARG(3);
  uint8_t *tmp = malloc(sz);
  for (uint32_t i = 1; i < n; i++)
    for (uint32_t j = i; j > 0 && (int32_t)rt_call(cmp, 2, base + (j - 1) * sz, base + j * sz) > 0; j--) {
      memcpy(tmp, (void *)P(base + j * sz), sz);
      memcpy((void *)P(base + j * sz), (void *)P(base + (j - 1) * sz), sz);
      memcpy((void *)P(base + (j - 1) * sz), tmp, sz);
    }
  free(tmp);
}
void imp_bsearch(void) {
  uint32_t key = ARG(0), base = ARG(1), n = ARG(2), sz = ARG(3), cmp = ARG(4);
  uint32_t lo = 0, hi = n;
  while (lo < hi) {
    uint32_t mid = (lo + hi) / 2; int32_t c = (int32_t)rt_call(cmp, 2, key, base + mid * sz);
    if (!c) { g_eax = base + mid * sz; return; }
    if (c < 0) hi = mid; else lo = mid + 1;
  }
  g_eax = 0;
}

// ---------------------------------------------------------------- dynamic linker
void imp_dlopen(void) { g_eax = 0x1; }
void imp_dlsym(void) { g_eax = rt_export(gstr(ARG(1))); LOGI("dlsym(%s) -> %x\n", gstr(ARG(1)), g_eax); }
void imp_dlclose(void) { g_eax = 0; }
void imp_dl_iterate_phdr(void) { g_eax = 0; }

// ---------------------------------------------------------------- JNI: fake JavaVM / JNIEnv
// Every slot is a host thunk; unimplemented ones log their index (see jni.h ordering) and abort.
#define JNI_SLOTS 233
extern uint32_t jvm, jenv;
void jni_init(void);
uint32_t jni_new_string(const char *s);
uint32_t jni_new_object(const char *cls);
static void jni_unimpl(int slot) { LOGI("JNI slot %d not implemented\n", slot); rt_trap(0); }
#define S(n) static void jni_##n(void) { jni_unimpl(n); }
#define S10(a) S(a##0) S(a##1) S(a##2) S(a##3) S(a##4) S(a##5) S(a##6) S(a##7) S(a##8) S(a##9)
S10() S10(1) S10(2) S10(3) S10(4) S10(5) S10(6) S10(7) S10(8) S10(9) S10(10) S10(11) S10(12) S10(13) S10(14)
S10(15) S10(16) S10(17) S10(18) S10(19) S10(20) S10(21) S10(22) S(230) S(231) S(232)
#undef S
#define E(n) jni_##n,
#define E10(a) E(a##0) E(a##1) E(a##2) E(a##3) E(a##4) E(a##5) E(a##6) E(a##7) E(a##8) E(a##9)
static void (*jni_default[JNI_SLOTS])(void) = { E10() E10(1) E10(2) E10(3) E10(4) E10(5) E10(6) E10(7) E10(8) E10(9)
  E10(10) E10(11) E10(12) E10(13) E10(14) E10(15) E10(16) E10(17) E10(18) E10(19) E10(20) E10(21) E10(22) E(230) E(231) E(232) };
// note: jni_0..9 come from S10() with empty prefix

void (*jni_default_slot(int slot))(void) { return jni_default[slot]; }

// ---------------------------------------------------------------- Android assets (APK assets/ dir)
typedef struct { FILE *f; uint32_t len; } gasset;
void imp_AAssetManager_fromJava(void) { g_eax = 0xA55E7000u; }
void imp_AAssetManager_open(void) {
  char path[1024]; snprintf(path, sizeof path, "%s/%s", asset_root, gstr(ARG(1)));
  FILE *f = fopen(path, "rb");
  if (!f) { if (getenv("ASSET_TRACE")) LOGI("asset miss: %s\n", gstr(ARG(1))); g_eax = 0; return; }
  gasset *a = calloc(1, sizeof *a); a->f = f; fseek(f, 0, SEEK_END); a->len = ftell(f); fseek(f, 0, SEEK_SET);
  if (getenv("ASSET_TRACE")) LOGI("asset open: %s (%u)\n", gstr(ARG(1)), a->len);
  g_eax = (uint32_t)(uintptr_t)a;
}
void imp_AAsset_close(void) { gasset *a = (gasset *)P(ARG(0)); if (a) { fclose(a->f); free(a); } }
void imp_AAsset_getLength(void) { g_eax = ((gasset *)P(ARG(0)))->len; }
void imp_AAsset_getRemainingLength(void) { gasset *a = (gasset *)P(ARG(0)); g_eax = a->len - ftell(a->f); }
void imp_AAsset_read(void) { gasset *a = (gasset *)P(ARG(0)); g_eax = fread(ARGP(1), 1, ARG(2), a->f); }
void imp_AAsset_seek(void) { gasset *a = (gasset *)P(ARG(0)); fseek(a->f, (int32_t)ARG(1), ARG(2)); g_eax = ftell(a->f); }

// ---------------------------------------------------------------- boot
int rt_load(const uint8_t *image, uint32_t len);
void rt_run_init_array(void);

static uint32_t native(const char *name) {
  char full[256]; snprintf(full, sizeof full, "Java_com_supercell_titan_GameApp_%s", name);
  uint32_t f = rt_export(full); if (!f) { LOGI("missing native %s\n", full); rt_trap(0); }
  return f;
}
static uint32_t gameapp_cls;
#define CALLN(name, ...) rt_call(native(name), __VA_ARGS__)

// Host-side game loop step, exported for the browser's requestAnimationFrame.
int game_frames;
EMSCRIPTEN_KEEPALIVE int game_frame(void) {
  int r = (int)CALLN("update", 2, jenv, gameapp_cls) & 0xFF;
  game_frames++;
  return r;
}

#ifdef WEB_BUILD
#include <emscripten/html5.h>
// Browser input. index.html writes the live pointers into web_touches: [count, (x, y, id) * 10] in
// game pixels (720x1280, y down). Each frame we hand the game the current set, as GameApp does.
static int32_t web_touches[1 + 30];
EMSCRIPTEN_KEEPALIVE void web_set_touch(int i, int x, int y, int id) { if (i >= 0 && i < 10) { web_touches[1 + 3 * i] = x; web_touches[2 + 3 * i] = y; web_touches[3 + 3 * i] = id; } }
EMSCRIPTEN_KEEPALIVE void web_touch_count(int n) { web_touches[0] = n; }
extern int ui_kb_req, ui_kb_max, ui_kb_native, ui_kb_hide, ui_dlg_pending; extern char ui_kb_text[512], ui_dlg[5][1024];
static int touches_were_down;
static void web_input(void) {
  int n = web_touches[0]; if (n > 10) n = 10;
  if (n || touches_were_down) {
    CALLN("clearTouches", 2, jenv, gameapp_cls);
    for (int i = 0; i < n; i++)
      CALLN("setTouch", 6, jenv, gameapp_cls, web_touches[1 + 3 * i], web_touches[2 + 3 * i], web_touches[3 + 3 * i] + 1, 0);   // (x, y, pointer id, unused)
    touches_were_down = n;
  }
  // Text input and dialogs use non-blocking HTML overlays (index.html: Module.uiKeyboard / uiDialog).
  if (ui_kb_req) {
    ui_kb_req = 0;
    EM_ASM({ if (Module.uiKeyboard) Module.uiKeyboard(UTF8ToString($0), $1, $2); }, ui_kb_text, ui_kb_max, ui_kb_native);
  }
  if (ui_kb_hide) { ui_kb_hide = 0; EM_ASM({ if (Module.uiKeyboardHide) Module.uiKeyboardHide(); }); }
  char *r = (char *)EM_ASM_PTR({
    var v = Module.__kbResult; if (v === undefined) return 0;
    Module.__kbResult = undefined; if (v === null) v = "\u0001";
    var n = lengthBytesUTF8(v) + 1; var p = _malloc(n); stringToUTF8(v, p, n); return p;
  });
  if (r) {
    if (r[0] == 1) CALLN("inputKeyboardDismissed", 2, jenv, gameapp_cls);
    else {
      extern uint32_t jni_new_string(const char *);
      CALLN("inputTextChanged", 3, jenv, gameapp_cls, jni_new_string(r));
      int len = EM_ASM_INT({ return UTF8ToString($0).length; }, r);   // caret at end (UTF-16 units, like Android)
      CALLN("inputSelectionChanged", 4, jenv, gameapp_cls, len, len);
    }
    free(r);
  }
  if (EM_ASM_INT({ return Module.__kbOk ? (Module.__kbOk = 0) + 1 : 0; })) {
    CALLN("inputOkPressed", 2, jenv, gameapp_cls);
  }
  if (ui_dlg_pending < 0) { int id = -ui_dlg_pending; ui_dlg_pending = 0; CALLN("dialogDismissed", 4, jenv, gameapp_cls, id, 0); }
  if (ui_dlg_pending) {
    int id = ui_dlg_pending; ui_dlg_pending = 0;
    EM_ASM({ if (Module.uiDialog) Module.uiDialog($0, UTF8ToString($1), UTF8ToString($2), UTF8ToString($3), UTF8ToString($4), UTF8ToString($5)); },
           id, ui_dlg[0], ui_dlg[1], ui_dlg[2], ui_dlg[3], ui_dlg[4]);
  }
  int dres = EM_ASM_INT({ var d = Module.__dlgResult; if (!d) return 0; Module.__dlgResult = null; return (d.id << 4) | d.button; });
  if (dres) CALLN("dialogDismissed", 4, jenv, gameapp_cls, dres >> 4, dres & 15);
}
// News Royale: the game's WebView becomes an iframe over the canvas (Module.uiWebView in index.html).
extern int ui_wv_dirty, ui_wv_x, ui_wv_y, ui_wv_w, ui_wv_h, ui_wv_loaded; extern char ui_wv_url[1024];
static void web_webview(void) {
  if (ui_wv_dirty) {
    ui_wv_dirty = 0;
    EM_ASM({ if (Module.uiWebView) Module.uiWebView(UTF8ToString($0), $1, $2, $3, $4); }, ui_wv_url, ui_wv_x, ui_wv_y, ui_wv_w, ui_wv_h);
  }
  if (ui_wv_loaded) {   // tell the game the page loaded so it drops its spinner
    ui_wv_loaded = 0;
    extern uint32_t jni_new_string(const char *);
    uint32_t a = rt_export("Java_com_supercell_titan_TitanWebView_onPageStarted"), b = rt_export("Java_com_supercell_titan_TitanWebView_onPageFinished");
    if (a) rt_call(a, 3, jenv, gameapp_cls, jni_new_string(ui_wv_url));
    if (b) rt_call(b, 3, jenv, gameapp_cls, jni_new_string(ui_wv_url));
  }
}
// Live battle score from the client's own simulation, read every 10 frames: the page reports it to the server, which
// decides the match result from it. Ghidra addresses minus 0x10000: FUN_000f866a = battle screen (0 outside battles),
// FUN_000fb400 / FUN_000fae66 = own / enemy player, FUN_0020a256 = player's crowns (the end screen's finalScore).
static int web_score = -1;
static void web_battle_poll(void) {
  static int tick; if (++tick % 10) return;
  uint32_t ctx = rt_call(B + 0xE866A, 0);
  if (!ctx) { web_score = -1; return; }
  uint32_t w = M32(ctx + 0x54); if (!w || !M32(w + 0x78)) return;
  uint32_t me = rt_call(B + 0xEB400, 2, ctx, 0), op = rt_call(B + 0xEAE66, 2, ctx, 0);
  if (!me || !op || !M32(me + 0x70) || !M32(op + 0x70)) return;
  int a = (int)rt_call(B + 0x1FA256, 1, me), b = (int)rt_call(B + 0x1FA256, 1, op);
  if (a >= 0 && a <= 3 && b >= 0 && b <= 3) web_score = a * 16 + b;
}
EMSCRIPTEN_KEEPALIVE int web_battle_score(void) { return web_score; }   // -1 = not in a battle, else mine * 16 + theirs
static void web_frame(void) { web_input(); web_webview(); game_frame(); web_battle_poll(); }
#endif

int main(int argc, char **argv) {
  const char *img_path = getenv("LIBG_IMG") ? getenv("LIBG_IMG") : "/libg.img";
  if (getenv("ASSET_ROOT")) asset_root = getenv("ASSET_ROOT");
  if (getenv("FS_ROOT")) snprintf(fs_root, sizeof fs_root, "%s", getenv("FS_ROOT"));
  { char b[1024]; snprintf(b, sizeof b, "%s", fs_root); mkdir(b, 0755);
    snprintf(b, sizeof b, "%s/data", fs_root); mkdir(b, 0755); snprintf(b, sizeof b, "%s/cache", fs_root); mkdir(b, 0755); }
  FILE *f = fopen(img_path, "rb");
  if (!f) { LOGI("missing %s\n", img_path); return 1; }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t *img = malloc(n); fread(img, 1, n, f); fclose(f);
  init_data();
  uint32_t stack = galloc(8 << 20); g_esp = (stack + (8 << 20) - 64) & ~15u;
  g_gs = galloc(4096); W32(g_gs, g_gs); W32(g_gs + 4, 1); W32(g_gs + 0x14, 0x2B1D5EEDu);
  if (rt_load(img, n)) { LOGI("rt_load failed\n"); return 1; }
  free(img);
  LOGI("image loaded at %08x (%u bytes)\n", B, IMAGE_SIZE);
  rt_run_init_array();
  LOGI("init_array done (%u constructors)\n", INIT_COUNT);
  { extern int net_trace; net_trace = getenv("NET_TRACE") ? atoi(getenv("NET_TRACE")) : 0; }
  jni_init();
  LOGI("JNI_OnLoad -> %x\n", rt_call(rt_export("JNI_OnLoad"), 2, jvm, 0));

  int W = 720, Hh = 1280, dpi = 320;   // portrait, like a phone
#ifdef WEB_BUILD
  {
    EmscriptenWebGLContextAttributes at; emscripten_webgl_init_context_attributes(&at);
    at.majorVersion = 1; at.alpha = 0; at.depth = 0; at.stencil = 0; at.antialias = 0; at.preserveDrawingBuffer = 0;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE ctx = emscripten_webgl_create_context("#canvas", &at);
    if (ctx <= 0) { LOGI("WebGL context creation failed\n"); return 1; }
    emscripten_webgl_make_context_current(ctx);
    emscripten_set_canvas_element_size("#canvas", W, Hh);
  }
#endif
  gameapp_cls = jni_new_object("com/supercell/titan/GameApp");
  uint32_t assets = jni_new_object("android/content/res/AssetManager");
  uint32_t r = rt_call(native("createGameMain"), 12, jenv, gameapp_cls, assets,
                       jni_new_string("/data"), jni_new_string("/cache"), jni_new_string(""),
                       0u, 0u, W, Hh, dpi, 2);
  LOGI("createGameMain -> %x\n", r);
  LOGI("surface format %u depth %u stencil %u\n", CALLN("getSurfaceFormat", 2, jenv, gameapp_cls),
       CALLN("getDepthBits", 2, jenv, gameapp_cls), CALLN("getStencilBits", 2, jenv, gameapp_cls));
  r = CALLN("init", 5, jenv, gameapp_cls, W, Hh, jni_new_string(""));
  LOGI("init -> %u\n", r & 0xFF);
  CALLN("start", 3, jenv, gameapp_cls, jni_new_string(""));
  LOGI("start done\n");
#ifdef WEB_BUILD
  emscripten_set_main_loop(web_frame, 0, 0);   // requestAnimationFrame
  return 0;
#endif
  int frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 3;
  { void rt_register_thread(uint32_t); rt_register_thread(1); }
  extern int gl_draw_calls __attribute__((weak));
  for (int i = 0; i < frames; i++) {
    int before = &gl_draw_calls ? gl_draw_calls : 0;
    int r = game_frame();
    usleep(16667);   // real-time pacing: the engine advances on wall-clock time
    if (i < 5 || i % 50 == 0 || i == frames - 1)
      LOGI("update #%d -> %d  draws=%d\n", i, r, (&gl_draw_calls ? gl_draw_calls : 0) - before);
  }
  if (getenv("DUMP_THREADS")) { void rt_dump_threads(void); rt_dump_threads(); }
  return 0;
}



static void stat_out(uint32_t a, struct stat *st) {
  memset((void *)P(a), 0, 96);
  W64(a + 0, st->st_dev); W32(a + 12, (uint32_t)st->st_ino); W32(a + 16, st->st_mode); W32(a + 20, st->st_nlink);
  W64(a + 44, st->st_size); W32(a + 52, 4096); W64(a + 56, (st->st_size + 511) / 512);
  W32(a + 64, (uint32_t)st->st_atime); W32(a + 72, (uint32_t)st->st_mtime); W32(a + 80, (uint32_t)st->st_ctime);
  W64(a + 88, st->st_ino);
}
static uint32_t ret_errno(int r) { if (r < 0) g_errno = linux_errno(errno); return (uint32_t)r; }
void imp_stat(void) { char b[1024]; struct stat st; int r = stat(gpath(ARG(0), b), &st); if (!r) stat_out(ARG(1), &st); g_eax = ret_errno(r); }
void imp_fstat(void) { struct stat st; int r = fstat(ARG(0), &st); if (!r) stat_out(ARG(1), &st); g_eax = ret_errno(r); }
void imp_open(void) { char b[1024]; g_eax = ret_errno(open(gpath(ARG(0), b), ARG(1), ARG(2))); }
void imp_openat(void) { char b[1024]; g_eax = ret_errno(open(gpath(ARG(1), b), ARG(2), ARG(3))); }
void imp_mkdir(void) { char b[1024]; g_eax = ret_errno(mkdir(gpath(ARG(0), b), ARG(1))); }
void imp_remove(void) { char b[1024]; g_eax = ret_errno(remove(gpath(ARG(0), b))); }
void imp_rename(void) { char a[1024], b[1024]; g_eax = ret_errno(rename(gpath(ARG(0), a), gpath(ARG(1), b))); }
void imp_opendir(void) { char b[1024]; DIR *d = opendir(gpath(ARG(0), b)); if (!d) g_errno = linux_errno(errno); g_eax = (uint32_t)(uintptr_t)d; }
void imp_read(void) { g_eax = ret_errno(read(ARG(0), ARGP(1), ARG(2))); }
void imp_write(void) { g_eax = ret_errno(write(ARG(0), ARGP(1), ARG(2))); }
int net_close(uint32_t fd); int net_fcntl(uint32_t fd, uint32_t cmd, uint32_t arg);
void imp_close(void) { if (!net_close(ARG(0))) { g_eax = 0; return; } g_eax = ret_errno(close(ARG(0))); }
void imp_fcntl(void) { int r = net_fcntl(ARG(0), ARG(1), ARG(2)); if (r != -2) { g_eax = r; return; } g_eax = ret_errno(fcntl(ARG(0), ARG(1), ARG(2))); }
static uint32_t dirent_buf;
static void dirent_out(uint32_t a, struct dirent *e) {
  memset((void *)P(a), 0, 280);
  W64(a, e->d_ino); W16(a + 16, 280); W8(a + 18, e->d_type);
  strncpy((char *)P(a + 19), e->d_name, 255);
}
void imp_readdir(void) {
  struct dirent *e = readdir((DIR *)P(ARG(0)));
  if (!e) { g_eax = 0; return; }
  if (!dirent_buf) dirent_buf = galloc(280);
  dirent_out(dirent_buf, e); g_eax = dirent_buf;
}
void imp_readdir_r(void) {
  struct dirent *e = readdir((DIR *)P(ARG(0)));
  if (e) { dirent_out(ARG(1), e); W32(ARG(2), ARG(1)); } else W32(ARG(2), 0);
  g_eax = 0;
}

// ---------------------------------------------------------------- process / memory / misc
void imp_popen(void) { g_eax = 0; }           // no shell: root/emulator probes see nothing
void imp_pclose(void) { g_eax = (uint32_t)-1; }
void imp_mmap(void) {                          // anonymous mappings only
  uint32_t len = ARG(1), flags = ARG(3);
  void *p = aligned_alloc(4096, (len + 4095) & ~4095u); memset(p, 0, len);
  if (!(flags & 0x20)) {                       // file mapping: read a private copy
    int fd = (int)ARG(4); off_t cur = lseek(fd, 0, SEEK_CUR);
    lseek(fd, (off_t)ARG(5), SEEK_SET);
    size_t got = 0; ssize_t r;
    while (got < len && (r = read(fd, (char *)p + got, len - got)) > 0) got += r;
    lseek(fd, cur, SEEK_SET);
  }
  g_eax = (uint32_t)(uintptr_t)p;
}
void imp_munmap(void) { free(ARGP(0)); g_eax = 0; }
void imp_ioctl(void) { g_eax = (uint32_t)-1; g_errno = 22; }
void imp_if_indextoname(void) { g_eax = 0; }
// scanf: every conversion takes a pointer, and guest pointers are host pointers — forward up to 16.
#define P16(b) ARGP(b),ARGP(b+1),ARGP(b+2),ARGP(b+3),ARGP(b+4),ARGP(b+5),ARGP(b+6),ARGP(b+7),\
               ARGP(b+8),ARGP(b+9),ARGP(b+10),ARGP(b+11),ARGP(b+12),ARGP(b+13),ARGP(b+14),ARGP(b+15)
void imp_sscanf(void) { g_eax = sscanf(gstr(ARG(0)), gstr(ARG(1)), P16(2)); }
void imp_vsscanf(void) {
  uint32_t ap = ARG(2); void *p[16];
  for (int i = 0; i < 16; i++) p[i] = (void *)P(M32(ap + 4 * i));
  g_eax = sscanf(gstr(ARG(0)), gstr(ARG(1)), p[0],p[1],p[2],p[3],p[4],p[5],p[6],p[7],p[8],p[9],p[10],p[11],p[12],p[13],p[14],p[15]);
}
void imp_strftime(void) {
  uint32_t a = ARG(3); struct tm t = {0};
  t.tm_sec = M32(a); t.tm_min = M32(a + 4); t.tm_hour = M32(a + 8); t.tm_mday = M32(a + 12); t.tm_mon = M32(a + 16);
  t.tm_year = M32(a + 20); t.tm_wday = M32(a + 24); t.tm_yday = M32(a + 28); t.tm_isdst = M32(a + 32);
  g_eax = strftime(ARGP(0), ARG(1), gstr(ARG(2)), &t);
}

void imp_socketpair(void) {   // used as a wake-up channel by the network thread
  int fd[2]; int r = pipe(fd);
  // The game writes wake-ups into sv[0] and selects on sv[1], so hand out the pipe's write end first.
  if (!r) { W32(ARG(3), fd[1]); W32(ARG(3) + 4, fd[0]); }
  g_eax = ret_errno(r);
}

