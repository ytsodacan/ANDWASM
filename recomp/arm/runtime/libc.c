// Hand-written bionic (32-bit ARM) libc imports: stdio, formatted output with AAPCS varargs, time,
// files, memory mapping, locale stubs. Straight 1:1 functions are generated (gen_imports.py).
#include "rt.h"
#pragma clang diagnostic ignored "-Wint-conversion"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wchar.h>
#include <time.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <ctype.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
static const char *gs(uint32_t a) { return (const char *)P(a); }
static uint32_t galloc(uint32_t n) { return (uint32_t)(uintptr_t)calloc(1, n); }

// ------------------------------------------------------------------ data imports
static uint32_t d_sF, d_stack_guard, d_sl_iid[8];
static const char *sl_names[8] = { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", "SL_IID_ENGINE", "SL_IID_METADATAEXTRACTION", "SL_IID_NULL",
                                   "SL_IID_PLAY", "SL_IID_PREFETCHSTATUS", "SL_IID_SEEK", "SL_IID_VOLUME" };
#define SF_SIZE 84u    // sizeof(FILE) in 32-bit bionic
uint32_t hle_data_import(const char *n) {
  if (!d_sF) {
    d_sF = galloc(3 * SF_SIZE);
    d_stack_guard = galloc(4); W32(d_stack_guard, 0x2B1D5EEDu);
    for (int i = 0; i < 8; i++) { d_sl_iid[i] = galloc(16); W32(d_sl_iid[i], 0x5100 + i); }
  }
  if (!strcmp(n, "__sF")) return d_sF;
  if (!strcmp(n, "__stack_chk_guard")) return d_stack_guard;
  for (int i = 0; i < 8; i++) if (!strcmp(n, sl_names[i])) {
    // the import is the SLInterfaceID variable itself: a pointer to the 16-byte interface id
    uint32_t var = galloc(4); W32(var, d_sl_iid[i]); return var;
  }
  return 0;
}
int rt_sl_iid_index(uint32_t id) { for (int i = 0; i < 8; i++) if (d_sl_iid[i] == id) return i; return -1; }

// ------------------------------------------------------------------ errno
static _Thread_local int g_errno;
void imp___errno(void) { RET((uint32_t)(uintptr_t)&g_errno); }
static void seterr(void) { g_errno = errno; }

// ------------------------------------------------------------------ paths
// Guest absolute paths live under /data in the browser FS; asset reads go to /assets.
const char *rt_fs_root(void) { static const char *r; if (!r) { r = getenv("ANDWASM_ROOT"); if (!r) r = ""; } return r; }
static const char *gpath(uint32_t a, char *buf, size_t n) {
  const char *p = gs(a);
  if (!strncmp(p, "assets/", 7)) { snprintf(buf, n, "%s/assets/%s", rt_fs_root(), p + 7); return buf; }
  if (p[0] == '/' && *rt_fs_root() && strncmp(p, "/dev/", 5) && strncmp(p, "/proc/", 6)) { snprintf(buf, n, "%s%s", rt_fs_root(), p); return buf; }
  return p;
}

// ------------------------------------------------------------------ varargs readers
typedef struct { int mode; uint32_t w; uint32_t ap; } areader;   // mode 0: argument words from w; 1: va_list pointer
static uint32_t rd32(areader *r) {
  if (r->mode == 0) { uint32_t w = r->w++; return ARGW(w); }
  uint32_t v = M32(r->ap); r->ap += 4; return v;
}
static uint64_t rd64(areader *r) {
  if (r->mode == 0) { if (r->w & 1) r->w++; uint64_t v = (uint64_t)ARGW(r->w) | ((uint64_t)ARGW(r->w + 1) << 32); r->w += 2; return v; }
  r->ap = (r->ap + 7) & ~7u; uint64_t v = M64(r->ap); r->ap += 8; return v;
}
static areader words(uint32_t w) { areader r = { 0, w, 0 }; return r; }
static areader valist(uint32_t ap) { areader r = { 1, 0, ap }; return r; }

// printf-style formatting of guest arguments into out (returns the full length, like vsnprintf).
static int gfmt(char *out, size_t cap, const char *f, areader *ar) {
  size_t o = 0; char spec[32];
  #define PUT(s, n) do { size_t n_ = (n); for (size_t k_ = 0; k_ < n_; k_++) { if (o + 1 < cap) out[o] = (s)[k_]; o++; } } while (0)
  while (*f) {
    if (*f != '%') { PUT(f, 1); f++; continue; }
    const char *st = f++;
    if (*f == '%') { PUT("%", 1); f++; continue; }
    int sl = 0; spec[sl++] = '%';
    while (*f && strchr("-+ #0'", *f)) { if (sl < 20) spec[sl++] = *f; f++; }
    if (*f == '*') { int v = (int)rd32(ar); sl += snprintf(spec + sl, 12, "%d", v); f++; } else while (isdigit((unsigned char)*f)) { if (sl < 20) spec[sl++] = *f; f++; }
    if (*f == '.') { spec[sl++] = '.'; f++; if (*f == '*') { int v = (int)rd32(ar); sl += snprintf(spec + sl, 12, "%d", v); f++; } else while (isdigit((unsigned char)*f)) { if (sl < 24) spec[sl++] = *f; f++; } }
    int lng = 0;                                   // 0 int, 1 long(32), 2 long long, -1 short/char
    while (*f && strchr("hlLqjzt", *f)) { if (*f == 'l') lng++; else if (*f == 'L' || *f == 'q' || *f == 'j') lng = 2; else if (*f == 'h') lng = -1; f++; }
    char cv = *f ? *f++ : 0;
    char tmp[512]; int n = 0;
    switch (cv) {
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'c':
        if (lng >= 2) { spec[sl++] = 'l'; spec[sl++] = 'l'; spec[sl++] = cv; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, (long long)rd64(ar)); }
        else { spec[sl++] = cv; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, (int)rd32(ar)); }
        break;
      case 'p': n = snprintf(tmp, sizeof tmp, "0x%x", rd32(ar)); break;
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
        uint64_t q = rd64(ar); double d; memcpy(&d, &q, 8);
        spec[sl++] = cv; spec[sl] = 0; n = snprintf(tmp, sizeof tmp, spec, d); break; }
      case 's': {
        uint32_t a = rd32(ar); const char *s = a ? gs(a) : "(null)";
        if (lng >= 1) {   // %ls: wide string
          static _Thread_local char nb[2048]; size_t k = 0; const uint32_t *ws = (const uint32_t *)P(a);
          for (; a && ws[k] && k < sizeof nb - 1; k++) nb[k] = ws[k] < 128 ? (char)ws[k] : '?'; nb[k] = 0; s = nb;
        }
        spec[sl++] = 's'; spec[sl] = 0;
        if (strlen(s) < sizeof tmp - 8 || sl > 2) { n = snprintf(tmp, sizeof tmp, spec, s); if ((size_t)n < sizeof tmp) { PUT(tmp, (size_t)n); continue; } }
        PUT(s, strlen(s)); continue; }
      case 'n': { uint32_t a = rd32(ar); if (a) W32(a, (uint32_t)o); continue; }
      default: PUT(st, (size_t)(f - st)); continue;
    }
    if (n > 0) PUT(tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
  }
  if (cap) out[o < cap ? o : cap - 1] = 0;
  return (int)o;
  #undef PUT
}

// ------------------------------------------------------------------ FILE
static FILE *hf(uint32_t g) {
  if (g >= d_sF && g < d_sF + 3 * SF_SIZE) { uint32_t k = (g - d_sF) / SF_SIZE; return k == 0 ? stdin : k == 1 ? stdout : stderr; }
  return (FILE *)P(g);
}
void imp_fopen(void) {
  char b[1024]; const char *p = gpath(ARG(0), b, sizeof b);
  FILE *f = fopen(p, gs(ARG(1)));
  if (!f) seterr();
  if (getenv("FILE_TRACE")) LOGI("fopen(%s, %s) -> %p\n", p, gs(ARG(1)), (void *)f);
  RET((uint32_t)(uintptr_t)f);
}
void imp_fclose(void) { FILE *f = hf(ARG(0)); RET(f == stdin || f == stdout || f == stderr ? 0 : fclose(f)); }
void imp_fread(void) { RET(fread(ARGP(0), ARG(1), ARG(2), hf(ARG(3)))); }
void imp_fwrite(void) { RET(fwrite(ARGP(0), ARG(1), ARG(2), hf(ARG(3)))); }
void imp_fseek(void) { RET(fseek(hf(ARG(0)), (int32_t)ARG(1), ARG(2))); }
void imp_fseeko(void) { RET(fseeko(hf(ARG(0)), (int32_t)ARG(1), ARG(2))); }
void imp_ftell(void) { RET((uint32_t)ftell(hf(ARG(0)))); }
void imp_ftello(void) { RET((uint32_t)ftello(hf(ARG(0)))); }
void imp_fflush(void) { RET(fflush(ARG(0) ? hf(ARG(0)) : NULL)); }
void imp_fgets(void) { RET((uint32_t)(uintptr_t)fgets(ARGP(0), ARG(1), hf(ARG(2)))); }
void imp_fputc(void) { RET(fputc(ARG(0), hf(ARG(1)))); }
void imp_fputs(void) { RET(fputs(gs(ARG(0)), hf(ARG(1)))); }
void imp_feof(void) { RET(feof(hf(ARG(0)))); }
void imp_ferror(void) { RET(ferror(hf(ARG(0)))); }
void imp_clearerr(void) { clearerr(hf(ARG(0))); }
void imp_fileno(void) { RET(fileno(hf(ARG(0)))); }
void imp_getc(void) { RET(getc(hf(ARG(0)))); }
void imp_getc_unlocked(void) { RET(getc(hf(ARG(0)))); }
void imp_ungetc(void) { RET(ungetc(ARG(0), hf(ARG(1)))); }
void imp_putchar(void) { RET(putchar(ARG(0))); }
void imp_puts(void) { RET(puts(gs(ARG(0)))); }
void imp_perror(void) { LOGI("%s: %s\n", ARG(0) ? gs(ARG(0)) : "", strerror(g_errno)); }
void imp_remove(void) { char b[1024]; RET(remove(gpath(ARG(0), b, sizeof b))); }
void imp_unlink(void) { char b[1024]; RET(unlink(gpath(ARG(0), b, sizeof b))); }

// ------------------------------------------------------------------ formatted output
void imp_printf(void) { char b[8192]; areader r = words(1); int n = gfmt(b, sizeof b, gs(ARG(0)), &r); fputs(b, stdout); RET(n); }
void imp_vprintf(void) { char b[8192]; areader r = valist(ARG(1)); int n = gfmt(b, sizeof b, gs(ARG(0)), &r); fputs(b, stdout); RET(n); }
void imp_fprintf(void) { char b[8192]; areader r = words(2); int n = gfmt(b, sizeof b, gs(ARG(1)), &r); fputs(b, hf(ARG(0))); RET(n); }
void imp_vfprintf(void) { char b[8192]; areader r = valist(ARG(2)); int n = gfmt(b, sizeof b, gs(ARG(1)), &r); fputs(b, hf(ARG(0))); RET(n); }
void imp_sprintf(void) { areader r = words(2); RET(gfmt((char *)P(ARG(0)), 1u << 30, gs(ARG(1)), &r)); }
void imp_snprintf(void) { areader r = words(3); RET(gfmt((char *)P(ARG(0)), ARG(1), gs(ARG(2)), &r)); }
void imp_vsnprintf(void) { areader r = valist(ARG(3)); RET(gfmt((char *)P(ARG(0)), ARG(1), gs(ARG(2)), &r)); }
void imp_vasprintf(void) {
  areader r = valist(ARG(2)); char tmp[16]; int n = gfmt(tmp, 0, gs(ARG(1)), &r);
  char *b = malloc(n + 1); r = valist(ARG(2)); gfmt(b, n + 1, gs(ARG(1)), &r);
  W32(ARG(0), (uint32_t)(uintptr_t)b); RET(n);
}
void imp_swprintf(void) {   // narrow the wide format, format, widen the result
  char f[2048]; const uint32_t *wf = (const uint32_t *)P(ARG(2)); size_t k = 0;
  for (; wf[k] && k < sizeof f - 1; k++) f[k] = wf[k] < 128 ? (char)wf[k] : '?'; f[k] = 0;
  char b[4096]; areader r = words(3); int n = gfmt(b, sizeof b, f, &r);
  uint32_t out = ARG(0), cap = ARG(1); if (!cap) { RET(-1); return; }
  uint32_t m = (uint32_t)n < cap - 1 ? (uint32_t)n : cap - 1;
  for (uint32_t i = 0; i < m; i++) W32(out + 4 * i, (uint8_t)b[i]); W32(out + 4 * m, 0);
  RET((uint32_t)n < cap ? (uint32_t)n : (uint32_t)-1);
}
// sscanf/vsscanf for guest arguments: results are written straight into guest memory, so no host varargs.
static int scan(const char *s, const char *f, areader *r) {
  const char *s0 = s; int n = 0;
  while (*f) {
    if (isspace((unsigned char)*f)) { while (isspace((unsigned char)*s)) s++; f++; continue; }
    if (*f != '%' || f[1] == '%') { if (*f == '%') f++; if (*s != *f) break; s++; f++; continue; }
    f++;
    int skip = 0, width = 0, sz = 0;                  // sz: -2 hh, -1 h, 0 int, 1 l, 2 ll / L
    if (*f == '*') { skip = 1; f++; }
    while (isdigit((unsigned char)*f)) width = width * 10 + (*f++ - '0');
    for (;;) { if (*f == 'h') { sz--; f++; } else if (*f == 'l') { sz++; f++; } else if (*f == 'L' || *f == 'q' || *f == 'j') { sz = 2; f++; } else if (*f == 'z' || *f == 't') f++; else break; }
    char cv = *f++;
    if (cv == 'n') { if (!skip) W32(rd32(r), (uint32_t)(s - s0)); continue; }
    if (cv != 'c' && cv != '[') while (isspace((unsigned char)*s)) s++;
    if (!*s && cv != 'n') return n ? n : -1;
    char buf[512]; int lim = width ? width : (int)sizeof buf - 1; if (lim > (int)sizeof buf - 1) lim = sizeof buf - 1;
    if (getenv("SCANF_TRACE")) fprintf(stderr, "  scan: conv %c buf(C stack)=%p s=%p next arg=%x\n", cv, (void *)buf, (void *)s, r->mode ? M32(r->ap) : ARGW(r->w));
    if (cv == 'd' || cv == 'i' || cv == 'u' || cv == 'x' || cv == 'X' || cv == 'o' || cv == 'p') {
      int base = cv == 'x' || cv == 'X' || cv == 'p' ? 16 : cv == 'o' ? 8 : cv == 'i' ? 0 : 10;
      int k = 0; while (s[k] && k < lim) { buf[k] = s[k]; k++; } buf[k] = 0;
      char *end; unsigned long long v;
      if (cv == 'd' || cv == 'i') v = (unsigned long long)strtoll(buf, &end, base); else v = strtoull(buf, &end, base);
      if (end == buf) break;
      s += end - buf;
      if (getenv("SCANF_TRACE")) fprintf(stderr, "  scan: parsed %llu, sz=%d\n", v, sz);
      if (!skip) { uint32_t a = rd32(r); if (getenv("SCANF_TRACE")) fprintf(stderr, "  scan: store to %x\n", a);
        if (sz >= 2) W64(a, v); else if (sz == -1) W16(a, v); else if (sz == -2) W8(a, v); else W32(a, (uint32_t)v); n++; }
      if (getenv("SCANF_TRACE")) fprintf(stderr, "  scan: stored; rest of format \"%s\"\n", f);
    } else if (cv == 'f' || cv == 'e' || cv == 'g' || cv == 'E' || cv == 'G' || cv == 'a') {
      int k = 0; while (s[k] && k < lim) { buf[k] = s[k]; k++; } buf[k] = 0;
      char *end; double v = strtod(buf, &end);
      if (end == buf) break;
      s += end - buf;
      if (!skip) { uint32_t a = rd32(r); if (sz >= 1) WF64(a, v); else WF32(a, (float)v); n++; }
    } else if (cv == 's') {
      int k = 0; uint32_t a = skip ? 0 : rd32(r);
      while (s[k] && !isspace((unsigned char)s[k]) && k < (width ? width : 1 << 30)) { if (a) W8(a + k, s[k]); k++; }
      if (!k) break;
      if (a) { W8(a + k, 0); n++; }
      s += k;
    } else if (cv == 'c') {
      int cnt = width ? width : 1; uint32_t a = skip ? 0 : rd32(r);
      for (int k = 0; k < cnt; k++) { if (!s[k]) return n ? n : -1; if (a) W8(a + k, s[k]); }
      s += cnt; if (a) n++;
    } else if (cv == '[') {
      int neg = 0; char set[256] = {0};
      if (*f == '^') { neg = 1; f++; }
      if (*f == ']') { set[(unsigned char)']'] = 1; f++; }
      while (*f && *f != ']') {
        if (f[1] == '-' && f[2] && f[2] != ']') { for (int c = (unsigned char)f[0]; c <= (unsigned char)f[2]; c++) set[c] = 1; f += 3; }
        else set[(unsigned char)*f++] = 1;
      }
      if (*f == ']') f++;
      uint32_t a = skip ? 0 : rd32(r); int k = 0;
      while (s[k] && (set[(unsigned char)s[k]] != neg) && k < (width ? width : 1 << 30)) { if (a) W8(a + k, s[k]); k++; }
      if (!k) break;
      if (a) { W8(a + k, 0); n++; }
      s += k;
    } else break;
  }
  return n;
}
void imp_sscanf(void) {
  if (getenv("SCANF_TRACE")) fprintf(stderr, "sscanf(\"%.60s\", \"%s\") r2=%x r3=%x sp=%x heap=%x\n", gs(ARG(0)), gs(ARG(1)), g_r[2], g_r[3], g_r[13], (unsigned)(__builtin_wasm_memory_size(0) * 65536u));
  areader r = words(2); int n = scan(gs(ARG(0)), gs(ARG(1)), &r);
  if (getenv("SCANF_TRACE")) fprintf(stderr, "  -> %d\n", n);
  RET(n);
}
void imp_vsscanf(void) { areader r = valist(ARG(2)); RET(scan(gs(ARG(0)), gs(ARG(1)), &r)); }

// ------------------------------------------------------------------ android log / assert / abort
static const char *prio_name(uint32_t p) { static const char *n[] = { "?", "?", "V", "D", "I", "W", "E", "F" }; return p < 8 ? n[p] : "?"; }
void imp___android_log_print(void) {
  char b[4096]; areader r = words(3); gfmt(b, sizeof b, gs(ARG(2)), &r);
  LOGI("[%s/%s] %s\n", prio_name(ARG(0)), ARG(1) ? gs(ARG(1)) : "", b);
}
void imp___android_log_write(void) { LOGI("[%s/%s] %s\n", prio_name(ARG(0)), ARG(1) ? gs(ARG(1)) : "", ARG(2) ? gs(ARG(2)) : ""); }
void imp___android_log_assert(void) {
  char b[2048] = ""; if (ARG(2)) { areader r = words(3); gfmt(b, sizeof b, gs(ARG(2)), &r); }
  LOGI("[assert/%s] %s %s\n", ARG(1) ? gs(ARG(1)) : "", ARG(0) ? gs(ARG(0)) : "", b); rt_trap(0);
}
void imp___assert2(void) { LOGI("assertion failed: %s (%s:%d %s)\n", gs(ARG(3)), gs(ARG(0)), (int)ARG(1), gs(ARG(2))); rt_trap(0); }
void imp___stack_chk_fail(void) { LOGI("stack smashing detected\n"); rt_trap(0); }
void imp_abort(void) { LOGI("guest abort()\n"); rt_trap(0); }
void imp_exit(void) { LOGI("guest exit(%d)\n", (int)ARG(0)); rt_trap(0); }
void imp_raise(void) { LOGI("guest raise(%d)\n", (int)ARG(0)); RET(0); }
void imp_kill(void) { RET(0); }
void imp___cxa_atexit(void) { RET(0); }
void imp___aeabi_atexit(void) { RET(0); }
void imp___cxa_finalize(void) {}
void imp___gnu_Unwind_Find_exidx(void) { W32(ARG(1), 0); RET(0); }   // no unwind tables: C++ throw ends in terminate
void imp_dladdr(void) { RET(0); }
void imp_eglGetProcAddress(void);
void imp___system_property_get(void) { W8(ARG(1), 0); RET(0); }
void imp_sigaction(void) { RET(0); }
void imp_sigemptyset(void) { if (ARG(0)) W32(ARG(0), 0); RET(0); }
void imp_prctl(void) { RET(0); }
void imp_ioctl(void) { RET(-1); }
void imp_fork(void) { RET(-1); }
void imp_execvp(void) { RET(-1); }
void imp_waitpid(void) { RET(-1); }
void imp_syscall(void) { if (ARG(0) == 224) { void imp_gettid(void); imp_gettid(); return; } RET(-1); }   // __NR_gettid
void imp_getrusage(void) { memset(ARGP(1), 0, 72); RET(0); }
void imp_sysconf(void) {
  uint32_t n = ARG(0);
  RET(n == 0x27 ? 4096 : (n == 0x60 || n == 0x61) ? 4 : n == 6 ? 100 : (uint32_t)-1);
}
// Environment seen by the guest. ARMHWCAP: SpiderMonkey's CPU-feature override; a CPU without VFP makes
// it keep its JITs off (generated ARM code cannot run in the lifted build), so scripts use the interpreter.
void imp_getenv(void) {
  const char *n = gs(ARG(0));
  static char hw[] = "armv7";
  if (!strcmp(n, "ARMHWCAP")) { RET((uint32_t)(uintptr_t)hw); return; }
  RET(0);
}
void imp_getcwd(void) { if (ARG(0)) { strncpy(ARGP(0), "/", ARG(1)); RET(ARG(0)); } else RET((uint32_t)(uintptr_t)strdup("/")); }
void imp_arc4random(void) { RET(((uint32_t)rand() << 16) ^ (uint32_t)rand()); }
void imp_strerror_r(void) { strncpy(ARGP(1), strerror(ARG(0)), ARG(2)); RET(0); }
void imp_strerror(void) { RET((uint32_t)(uintptr_t)strerror(ARG(0))); }
void imp_usleep(void) { usleep(ARG(0)); RET(0); }

// ------------------------------------------------------------------ qsort / bsearch with guest comparators
static _Thread_local uint32_t g_cmp;
static int cmp_tramp(const void *a, const void *b) { return (int)rt_call(g_cmp, 2, (uint32_t)(uintptr_t)a, (uint32_t)(uintptr_t)b); }
void imp_qsort(void) { uint32_t old = g_cmp; g_cmp = ARG(3); qsort(ARGP(0), ARG(1), ARG(2), cmp_tramp); g_cmp = old; }
void imp_bsearch(void) {
  uint32_t old = g_cmp; g_cmp = ARG(4);
  void *r = bsearch(ARGP(0), ARGP(1), ARG(2), ARG(3), cmp_tramp); g_cmp = old; RET((uint32_t)(uintptr_t)r);
}

// ------------------------------------------------------------------ time (32-bit time_t in the guest)
void imp_time(void) { uint32_t t = (uint32_t)time(NULL); if (ARG(0)) W32(ARG(0), t); RET(t); }
void imp_gettimeofday(void) {
  struct timeval tv; gettimeofday(&tv, NULL);
  if (ARG(0)) { W32(ARG(0), (uint32_t)tv.tv_sec); W32(ARG(0) + 4, (uint32_t)tv.tv_usec); }
  if (ARG(1)) { W32(ARG(1), 0); W32(ARG(1) + 4, 0); }
  RET(0);
}
void imp_clock_gettime(void) {
  struct timespec ts; clock_gettime(ARG(0) == 0 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &ts);
  W32(ARG(1), (uint32_t)ts.tv_sec); W32(ARG(1) + 4, (uint32_t)ts.tv_nsec); RET(0);
}
void imp_nanosleep(void) { uint32_t ts = ARG(0); usleep(M32(ts) * 1000000u + M32(ts + 4) / 1000u); RET(0); }
// struct tm has the same 44-byte layout in 32-bit bionic and wasm32, so guest pointers are used directly.
void imp_mktime(void) { RET((uint32_t)mktime((struct tm *)P(ARG(0)))); }
void imp_localtime_r(void) { time_t t = (int32_t)M32(ARG(0)); localtime_r(&t, (struct tm *)P(ARG(1))); RET(ARG(1)); }
void imp_gmtime_r(void) { time_t t = (int32_t)M32(ARG(0)); gmtime_r(&t, (struct tm *)P(ARG(1))); RET(ARG(1)); }
void imp_localtime(void) { static _Thread_local struct tm r; time_t t = (int32_t)M32(ARG(0)); localtime_r(&t, &r); RET((uint32_t)(uintptr_t)&r); }
void imp_gmtime(void) { static _Thread_local struct tm r; time_t t = (int32_t)M32(ARG(0)); gmtime_r(&t, &r); RET((uint32_t)(uintptr_t)&r); }
void imp_strftime(void) { RET(strftime(ARGP(0), ARG(1), gs(ARG(2)), (const struct tm *)P(ARG(3)))); }

// ------------------------------------------------------------------ file descriptors, stat, dirs
static int arm_oflags(uint32_t f) {   // ARM bionic O_* -> host
  int h = f & 3;
  if (f & 0100) h |= O_CREAT; if (f & 0200) h |= O_EXCL; if (f & 01000) h |= O_TRUNC; if (f & 02000) h |= O_APPEND;
  if (f & 04000) h |= O_NONBLOCK; if (f & 040000) h |= O_DIRECTORY;
  return h;
}
void imp_open(void) { char b[1024]; int r = open(gpath(ARG(0), b, sizeof b), arm_oflags(ARG(1)), ARG(2)); if (r < 0) seterr(); RET(r); }
void imp_read(void) { RET(read(ARG(0), ARGP(1), ARG(2))); }
void imp_write(void) {
  if (ARG(0) == 1 || ARG(0) == 2) { fwrite(ARGP(1), 1, ARG(2), ARG(0) == 1 ? stdout : stderr); RET(ARG(2)); return; }
  RET(write(ARG(0), ARGP(1), ARG(2)));
}
void imp_lseek(void) { RET((uint32_t)lseek(ARG(0), (int32_t)ARG(1), ARG(2))); }
static void stat_out(uint32_t a, struct stat *st) {   // 104-byte struct stat of 32-bit ARM bionic
  memset(P(a), 0, 104);
  W64(a + 0, st->st_dev); W32(a + 12, (uint32_t)st->st_ino); W32(a + 16, st->st_mode); W32(a + 20, st->st_nlink);
  W64(a + 48, (uint64_t)st->st_size); W32(a + 56, 4096); W64(a + 64, (uint64_t)(st->st_size + 511) / 512);
  W32(a + 72, (uint32_t)st->st_atime); W32(a + 80, (uint32_t)st->st_mtime); W32(a + 88, (uint32_t)st->st_ctime);
  W64(a + 96, st->st_ino);
}
void imp_stat(void) { char b[1024]; struct stat st; int r = stat(gpath(ARG(0), b, sizeof b), &st); if (!r) stat_out(ARG(1), &st); else seterr(); RET(r); }
void imp_fstat(void) { struct stat st; int r = fstat(ARG(0), &st); if (!r) stat_out(ARG(1), &st); else seterr(); RET(r); }
void imp_mkdir(void) { char b[1024]; int r = mkdir(gpath(ARG(0), b, sizeof b), ARG(1)); if (r) seterr(); RET(r); }
void imp_opendir(void) { char b[1024]; RET((uint32_t)(uintptr_t)opendir(gpath(ARG(0), b, sizeof b))); }
void imp_closedir(void) { RET(ARG(0) ? closedir((DIR *)P(ARG(0))) : 0); }

// ------------------------------------------------------------------ memory mapping
void imp_mmap(void) {
  uint32_t len = ARG(1), flags = ARG(3); int fd = (int)ARG(4); uint32_t off = ARGW(5);
  void *m = aligned_alloc(4096, (len + 4095) & ~4095u);
  if (!m) { RET(-1); return; }
  memset(m, 0, len);
  if (!(flags & 0x20) && fd >= 0) { lseek(fd, off, SEEK_SET); ssize_t k = 0, got; while ((size_t)k < len && (got = read(fd, (char *)m + k, len - k)) > 0) k += got; }
  RET((uint32_t)(uintptr_t)m);
}
void imp_munmap(void) { RET(0); }
void imp_mprotect(void) { RET(0); }
void imp_madvise(void) { RET(0); }
void imp_posix_memalign(void) { void *p = aligned_alloc(ARG(1) < 4 ? 4 : ARG(1), (ARG(2) + ARG(1) - 1) / ARG(1) * ARG(1)); W32(ARG(0), (uint32_t)(uintptr_t)p); RET(p ? 0 : 12); }
void imp_memalign(void) { uint32_t al = ARG(0) < 4 ? 4 : ARG(0); RET((uint32_t)(uintptr_t)aligned_alloc(al, (ARG(1) + al - 1) / al * al)); }

// ------------------------------------------------------------------ locale and multibyte (C/UTF-8 locale only)
void imp_setlocale(void) { static char c[] = "C"; RET((uint32_t)(uintptr_t)c); }
void imp_newlocale(void) { RET(1); }
void imp_freelocale(void) {}
void imp_uselocale(void) { RET(1); }
void imp_localeconv(void) {
  static uint32_t lc[24]; static char dot[] = ".", empty[] = "";
  if (!lc[0]) { lc[0] = (uint32_t)(uintptr_t)dot; for (int i = 1; i < 10; i++) lc[i] = (uint32_t)(uintptr_t)empty; memset(&lc[10], 127, 14 * 4); }
  RET((uint32_t)(uintptr_t)lc);
}
void imp_strtold_l(void) { retd(strtod(gs(ARG(0)), (char **)P(ARG(1)))); }
void imp_strtoll_l(void) { RET64(strtoll(gs(ARG(0)), (char **)P(ARG(1)), ARG(2))); }
void imp_strtoull_l(void) { RET64(strtoull(gs(ARG(0)), (char **)P(ARG(1)), ARG(2))); }
void imp_mbrtowc(void) { RET(mbrtowc((wchar_t *)P(ARG(0)), gs(ARG(1)), ARG(2), NULL)); }
void imp_mbrlen(void) { RET(mbrlen(gs(ARG(0)), ARG(1), NULL)); }
void imp_mbtowc(void) { RET(mbtowc((wchar_t *)P(ARG(0)), gs(ARG(1)), ARG(2))); }
void imp_wcrtomb(void) { RET(wcrtomb((char *)P(ARG(0)), (wchar_t)ARG(1), NULL)); }
void imp_mbsrtowcs(void) { const char *s = gs(M32(ARG(1))); size_t r = mbsrtowcs((wchar_t *)P(ARG(0)), &s, ARG(2), NULL); W32(ARG(1), (uint32_t)(uintptr_t)s); RET(r); }
void imp_mbsnrtowcs(void) { const char *s = gs(M32(ARG(1))); size_t r = mbsnrtowcs((wchar_t *)P(ARG(0)), &s, ARG(2), ARG(3), NULL); W32(ARG(1), (uint32_t)(uintptr_t)s); RET(r); }
void imp_wcsnrtombs(void) { const wchar_t *s = (const wchar_t *)P(M32(ARG(1))); size_t r = wcsnrtombs((char *)P(ARG(0)), &s, ARG(2), ARG(3), NULL); W32(ARG(1), (uint32_t)(uintptr_t)s); RET(r); }

// ------------------------------------------------------------------ networking: unavailable
void imp_socket(void) { g_errno = 97; RET(-1); }
void imp_bind(void) { RET(-1); }
void imp_listen(void) { RET(-1); }
void imp_accept(void) { RET(-1); }
void imp_send(void) { RET(-1); }
void imp_recv(void) { RET(-1); }
void imp_setsockopt(void) { RET(-1); }
void imp_select(void) { RET(0); }
void imp_getaddrinfo(void) { RET(-2); }   // EAI_NONAME
void imp_freeaddrinfo(void) {}
void imp_gai_strerror(void) { static char s[] = "no network"; RET((uint32_t)(uintptr_t)s); }
void imp_inet_ntop(void) { RET(0); }
void imp_inet_pton(void) { RET(0); }
void imp___FD_SET_chk(void) { uint32_t fd = ARG(0), set = ARG(1); W32(set + 4 * (fd / 32), M32(set + 4 * (fd / 32)) | (1u << (fd % 32))); }
void imp___FD_CLR_chk(void) { uint32_t fd = ARG(0), set = ARG(1); W32(set + 4 * (fd / 32), M32(set + 4 * (fd / 32)) & ~(1u << (fd % 32))); }
void imp___FD_ISSET_chk(void) { uint32_t fd = ARG(0), set = ARG(1); RET((M32(set + 4 * (fd / 32)) >> (fd % 32)) & 1); }
