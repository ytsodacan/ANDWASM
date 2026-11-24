// Core runtime: image load, relocation, dispatch, guest<->host calls.
#include "rt.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

uint32_t B;
_Thread_local uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, g_gs, g_fs;
_Thread_local xmm_t X[8];
const xmm_t XZERO;
_Thread_local double FST[8];
_Thread_local int FTOP;

#define MAX_HOST_FNS 1024
static uint32_t thunk_base, thunk_end;
static void (*host_fns[MAX_HOST_FNS])(void);
static int host_count;

uint32_t rt_thunk_base(void) { return thunk_base; }

_Thread_local uint32_t rt_ring[256];
_Thread_local uint32_t rt_ring_i;
// Every thread's call ring, for rt_dump_threads() (debug aid).
static struct { uint32_t *ring, *idx, id; } rings[80]; static int nrings;
void rt_register_thread(uint32_t id) { int i = __atomic_fetch_add(&nrings, 1, __ATOMIC_SEQ_CST); if (i < 80) { rings[i].ring = rt_ring; rings[i].idx = &rt_ring_i; rings[i].id = id; } }
void rt_dump_threads(void) {
  for (int t = 0; t < nrings && t < 80; t++) {
    fprintf(stderr, "thread #%u last calls:", rings[t].id);
    for (uint32_t i = 48; i > 0; i--) fprintf(stderr, " %x", rings[t].ring[(*rings[t].idx - i) & 255]);
    fprintf(stderr, "\n");
  }
}
// Debug aid for TRACE_FUNCS builds: at the battle-state decoder (guest F_28B9C2) print where the read
// position is in the incoming stream, so it can be compared with what the server wrote.
static void dump_stream(uint32_t st) {
  uint32_t pos = M32(st + 0x10), len = M32(st + 0x14), buf = M32(st + 0x1c);
  fprintf(stderr, "[stream] st=%x +0c=%u +10=%u +14=%u +18=%u +1c=%x\n[stream] bytes before pos:", st, M32(st + 0xc), pos, len, M32(st + 0x18), buf);
  for (uint32_t i = pos > 40 ? pos - 40 : 0; i < pos && buf; i++) fprintf(stderr, " %02x", *(uint8_t *)P(buf + i));
  fprintf(stderr, "\n[stream] bytes from pos:");
  for (uint32_t i = pos; i < pos + 48 && i < len && buf; i++) fprintf(stderr, " %02x", *(uint8_t *)P(buf + i));
  fprintf(stderr, "\n");
}
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif
void rt_trace_ret(uint32_t fn) {
#ifdef __EMSCRIPTEN__
  MAIN_THREAD_ASYNC_EM_ASM({ try { var b = (typeof location !== 'undefined' && location.protocol === 'https:') ? '/bridge' : 'http://127.0.0.1:8643';
    fetch(b + '/dbg?ret=' + $0.toString(16) + ',' + ($1|0), {mode:'no-cors', keepalive:true}).catch(function(){}); } catch(e){} }, fn, g_eax);
#endif
}
void rt_trace_hit(uint32_t fn) {
#ifdef __EMSCRIPTEN__
  if (fn == 0x3abbd0) {   // Piranha log function: forward the message text (capped)
    static int n; if (n++ < 40) { const char *m = (const char *)P(M32(g_esp + 4));
      if (m[0] == 'N' || m[0] == 'T') {   // string-decoder errors: report the guest call stack (image offsets)
        static char sb[1200]; int o = snprintf(sb, sizeof sb, "%s |", m);
        for (int i = 0; i < 160 && o < 1100; i++) { uint32_t v = M32(g_esp + 4 * i); if (v >= B && v < B + IMAGE_SIZE) o += snprintf(sb + o, sizeof sb - o, " %x", v - B); }
        uint32_t regs[64]; for (int k = 0; k < 64; k++) regs[k] = M32(g_esp + 4 * k);
        int found = 0;
        for (int k = 0; k < 64 && o < 1050 && found < 2; k++) { uint32_t st = regs[k]; if (st < 0x10000 || st > 0x7fff0000u || st > 0x1fffff00u) continue;
          uint32_t pos = M32(st + 0x10), len = M32(st + 0x14), buf = M32(st + 0x1c);
          if (!buf || buf > 0x1fffff00u || len < 8 || len > 4000000 || pos > len || pos < 8) continue; found++;
          o += snprintf(sb + o, sizeof sb - o, " ##r%d pos=%u len=%u before:", k, pos, len);
          for (uint32_t i = pos > 36 ? pos - 36 : 0; i < pos + 8 && i < len && o < 1150; i++) o += snprintf(sb + o, sizeof sb - o, "%02x", *(uint8_t *)P(buf + i)); }
        m = sb; }
      MAIN_THREAD_ASYNC_EM_ASM({ try { var b = (typeof location !== 'undefined' && location.protocol === 'https:') ? '/bridge' : 'http://127.0.0.1:8643';
        fetch(b + '/dbg?log=' + encodeURIComponent(UTF8ToString($0)), {mode:'no-cors', keepalive:true}).catch(function(){}); } catch(e){} }, m); }
    return;
  }
  MAIN_THREAD_ASYNC_EM_ASM({ try { var b = (typeof location !== 'undefined' && location.protocol === 'https:') ? '/bridge' : 'http://127.0.0.1:8643';
    fetch(b + '/dbg?m=' + [$0,$1,$2,$3,$4].map(function(x){return (x>>>0).toString(16);}).join(','), {mode:'no-cors', keepalive:true}).catch(function(){}); } catch(e){} },
    fn, M32(g_esp + 4), M32(g_esp + 8), M32(g_esp + 12), M32(g_esp + 16));
#endif
  if (fn == 0x2936c0) {   // purchase validator: show what it compares (Ghidra FUN_002a36c0)
    uint32_t cmd = M32(g_esp + 4), av = M32(g_esp + 8);
    uint32_t shop = rt_call(B + 0x27B1A4, 1, av);
    uint32_t idx = M32(cmd + 0x1c), data = M32(cmd + 0x18), amt = M32(cmd + 0x20);
    uint32_t cnt = shop ? M32(shop + 0x1a4) : 0xffffffffu;
    uint32_t item = (shop && (int)idx >= 0 && idx < cnt) ? M32(M32(shop + 0x19c) + idx * 4) : 0;
    fprintf(stderr, "[val] idx=%d amt=%d cmd.data=%x(id %d) shopCount=%d item=%x item.bought=%d item.data=%x(id %d)\n", (int)idx, (int)amt, data, data ? (int)M32(data + 0x10) : -1,
            (int)cnt, item, item ? (int)M32(item + 4) : -1, item ? M32(item + 0x20) : 0, item && M32(item + 0x20) ? (int)M32(M32(item + 0x20) + 0x10) : -1);
    fprintf(stderr, "[val] stack return addrs (image offsets, add 0x10000 for the decompiler):"); for (int i = 0; i < 80; i++) { uint32_t v = M32(g_esp + 4 * i); if (v >= B && v < B + IMAGE_SIZE) fprintf(stderr, " %x", v - B); } fprintf(stderr, "\n");
    for (uint32_t i = 0; shop && i < cnt && i < 8; i++) { uint32_t it = M32(M32(shop + 0x19c) + i * 4); fprintf(stderr, "[val]   shop[%u]=%x id=%d fields(+4..+24):", i, it, it && M32(it + 0x20) ? (int)M32(M32(it + 0x20) + 0x10) : -1);
      for (uint32_t o = 4; it && o <= 0x24; o += 4) fprintf(stderr, " %d", (int)M32(it + o)); fprintf(stderr, "\n"); }
  }
  if (fn == 0x290562) { uint32_t c = M32(g_esp + 4); fprintf(stderr, "[cmd] validate: cmd=%x type(+18)=%d idx(+1c)=%d amt(+20)=%d\n", c, (int)M32(c + 0x18), (int)M32(c + 0x1c), (int)M32(c + 0x20)); }
  if (fn == 0x293a0a) fprintf(stderr, "[cmd] APPLY purchase: cur=%d data=%x idx=%d amount=%d\n", (int)M32(g_esp + 20), M32(g_esp + 8), (int)M32(g_esp + 12), (int)M32(g_esp + 16));
  if (fn == 0x27b9c2 || fn == 0x27ba2a) { fprintf(stderr, "[stream] -- entering %s\n", fn == 0x27ba2a ? "fullUpdate" : "decodeOnce"); dump_stream(M32(g_esp + 8)); }
  fprintf(stderr, "[trace] F_%X  esp=%x  args: %x %x %x %x\n", fn, g_esp, M32(g_esp + 4), M32(g_esp + 8), M32(g_esp + 12), M32(g_esp + 16));
  for (int k = 1; k <= 2; k++) { uint32_t a = M32(g_esp + 4 * k); if (a > 0x10000 && a < 0x7fff0000) {
    const char *c = (const char *)P(a); fprintf(stderr, "    arg%d bytes:", k - 1);
    for (int j = 0; j < 24; j++) fprintf(stderr, "%c", c[j] >= 32 && c[j] < 127 ? c[j] : '.'); fprintf(stderr, "\n"); } }
}
static void dump_regs(void) {
  fprintf(stderr, "  last calls (oldest→newest):");
  for (uint32_t i = 64; i > 0; i--) fprintf(stderr, " %x", rt_ring[(rt_ring_i - i) & 255]);
  fprintf(stderr, "\n");
  fprintf(stderr, "  eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x B=%08x\n",
          g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi, B);
  fprintf(stderr, "  backtrace (guest return addresses on stack):");
  for (int i = 0; i < 64; i++) {
    uint32_t v = M32(g_esp + 4 * i);
    if (v >= B && v < B + IMAGE_SIZE) fprintf(stderr, " %x", v - B);
  }
  fprintf(stderr, "\n  stack strings:");
  for (int i = 0; i < 24; i++) {        // pointers on the stack that look like C strings
    uint32_t v = M32(g_esp + 4 * i); if (v < 0x10000 || v > 0x7FFFFFFF) continue;
    const char *c = (const char *)P(v); int n = 0;
    while (n < 48 && c[n] >= 32 && c[n] < 127) n++;
    if (n >= 3 && (c[n] == 0)) fprintf(stderr, " [esp+%x]=\"%.*s\"", 4 * i, n, c);
  }
  fprintf(stderr, "\n");
}
void rt_unsupported(uint32_t off) { fprintf(stderr, "unsupported instruction at %x\n", off); dump_regs(); abort(); }
void rt_fallthrough(uint32_t off) { fprintf(stderr, "fell off end of function %x\n", off); dump_regs(); abort(); }
void rt_trap(uint32_t off) { fprintf(stderr, "trap at %x\n", off); dump_regs(); abort(); }
void rt_missing_import(const char *name) {
  fprintf(stderr, "unimplemented import: %s\n", name); dump_regs(); abort();
}

static int find_func(uint32_t off) {
  uint32_t lo = 0, hi = FUNC_COUNT;
  while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (FUNC_ADDR[mid] < off) lo = mid + 1; else hi = mid; }
  return (lo < FUNC_COUNT && FUNC_ADDR[lo] == off) ? (int)lo : -1;
}

void rt_dispatch(uint32_t addr) {
  uint32_t off = addr - B;
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
  // Debug watch (RT_WATCH builds only): while Module.__rtWatch is set, indirect call targets are kept in window.__rtLog
  // (the latest 200k-300k), then sent to the local log server in one request with window.__rtFlush().
  if (EM_ASM_INT({ return Module.__rtWatch ? 1 : 0; })) EM_ASM({ var L = window.__rtLog = window.__rtLog || []; if (L.length >= 300000) L.splice(0, 100000); L.push(($0 >>> 0).toString(16) + ' ' + ($1 >>> 0).toString(16) + ' ' + ($2 >>> 0).toString(16) + ' ' + ($3 >>> 0).toString(16)); }, off, M32(g_esp + 4), M32(g_esp + 8), M32(g_esp + 12));
#endif
  if (off < IMAGE_SIZE) {
    int i = find_func(off);
    if (i >= 0) { FUNC_PTR[i](); return; }
    fprintf(stderr, "dispatch to unknown code offset %x\n", off); dump_regs(); abort();
  }
  if (addr >= thunk_base && addr < thunk_end) {
    uint32_t idx = (addr - thunk_base) / THUNK_STRIDE;
    void (*fn)(void) = idx < IMPORT_COUNT ? IMPORT_FN[idx] : host_fns[idx - IMPORT_COUNT];
    fn();
    g_esp += 4;  // host functions "return": pop the guest return address
    return;
  }
  fprintf(stderr, "dispatch to wild address %08x\n", addr); dump_regs(); abort();
}

#include <pthread.h>
static pthread_mutex_t host_lock = PTHREAD_MUTEX_INITIALIZER;
uint32_t rt_host_fn(void (*fn)(void)) {
  pthread_mutex_lock(&host_lock);
  if (host_count >= MAX_HOST_FNS) abort();
  uint32_t a = thunk_base + (IMPORT_COUNT + host_count) * THUNK_STRIDE;
  host_fns[host_count++] = fn;
  pthread_mutex_unlock(&host_lock);
  return a;
}
uint32_t rt_export(const char *name) {
  for (uint32_t i = 0; i < EXPORT_COUNT; i++)
    if (!strcmp(EXPORTS[i].name, name)) return B + EXPORTS[i].off;
  return 0;
}

uint32_t rt_call(uint32_t fn, int argc, ...) {
  va_list ap; va_start(ap, argc);
  uint32_t saved_esp = g_esp;
  g_esp -= 4 * argc;
  g_esp &= ~15u;
  for (int i = 0; i < argc; i++) W32(g_esp + 4 * i, va_arg(ap, uint32_t));
  va_end(ap);
  g_esp -= 4; W32(g_esp, 0xDEADBEEFu);  // fake return address
  rt_dispatch(fn);                      // guest RET pops it (or host thunk path pops it)
  g_esp = saved_esp;
  return g_eax;
}

double f80_load(uint32_t a) {
  uint64_t man = M64(a); uint16_t se = M16(a + 8);
  int sign = se >> 15, exp = se & 0x7FFF;
  if (exp == 0 && man == 0) return sign ? -0.0 : 0.0;
  if (exp == 0x7FFF) return (man << 1) ? NAN : (sign ? -INFINITY : INFINITY);
  double v = ldexp((double)man, exp - 16383 - 63);
  return sign ? -v : v;
}
void f80_store(uint32_t a, double v) {
  uint16_t sign = signbit(v) ? 0x8000 : 0;
  if (v == 0) { W64(a, 0); W16(a + 8, sign); return; }
  if (isnan(v)) { W64(a, 0xC000000000000000ull); W16(a + 8, 0x7FFF); return; }
  if (isinf(v)) { W64(a, 0x8000000000000000ull); W16(a + 8, sign | 0x7FFF); return; }
  int e; double m = frexp(fabs(v), &e);   // v = m * 2^e, m in [0.5,1)
  W64(a, (uint64_t)ldexp(m, 64)); W16(a + 8, sign | (uint16_t)(e - 1 + 16383));
}

// ---- boot ----
void rt_install_imports(void);  // hle.c: data imports / per-import setup
extern uint32_t hle_data_import(const char *name);  // returns guest addr of data object or 0

int rt_load(const uint8_t *image, uint32_t len) {
  uint32_t total = IMAGE_SIZE + (IMPORT_COUNT + MAX_HOST_FNS) * THUNK_STRIDE;
  uint8_t *mem = aligned_alloc(0x10000, (total + 0xFFFF) & ~0xFFFFu);
  if (!mem) return -1;
  memset(mem, 0, total);
  memcpy(mem, image, len < IMAGE_SIZE ? len : IMAGE_SIZE);
  B = (uint32_t)(uintptr_t)mem;
  thunk_base = B + IMAGE_SIZE;
  thunk_end = thunk_base + (IMPORT_COUNT + MAX_HOST_FNS) * THUNK_STRIDE;
  for (uint32_t i = 0; i < RELOC_COUNT; i++) {
    const reloc_t *r = &RELOCS[i];
    uint32_t p = B + r->off, s;
    if (r->imp >= 0) {
      s = hle_data_import(IMPORT_NAME[r->imp]);
      if (!s) s = thunk_base + r->imp * THUNK_STRIDE;
    } else s = B + r->val;
    switch (r->type) {
      case 8: W32(p, M32(p) + B); break;          // R_386_RELATIVE
      case 1: W32(p, M32(p) + s); break;          // R_386_32
      case 6: case 7: W32(p, s); break;           // GLOB_DAT / JMP_SLOT
      case 2: W32(p, M32(p) + s - p); break;      // PC32
      default: fprintf(stderr, "reloc type %d\n", r->type); return -1;
    }
  }
  return 0;
}

void rt_run_init_array(void) {
  for (uint32_t i = 0; i < INIT_COUNT; i++) rt_call(B + INIT_ARRAY[i], 0);
}

// ---- setjmp / longjmp -------------------------------------------------------------------------
// The lifted call site has already done SYNC_OUT with esp pointing at the guest jmp_buf argument
// (no return address pushed), so the callee-saved state below is exactly what setjmp would record.
#define MAX_JB 256
// Slots are keyed by the guest jmp_buf address and never explicitly freed (the guest has no "done"
// call), so when the table is full we recycle round-robin; a longjmp always targets a recent setjmp.
static _Thread_local struct { uint32_t key; jmp_buf jb; uint32_t ebx, esi, edi, ebp, esp; } jbs[MAX_JB];
static _Thread_local uint32_t jb_next;
jmp_buf *rt_setjmp_slot(uint32_t key) {
  int i, slot = -1;
  for (i = 0; i < MAX_JB; i++) { if (jbs[i].key == key) { slot = i; break; } if (!jbs[i].key && slot < 0) slot = i; }
  if (slot < 0) slot = jb_next++ % MAX_JB;
  jbs[slot].key = key;
  jbs[slot].ebx = g_ebx; jbs[slot].esi = g_esi; jbs[slot].edi = g_edi; jbs[slot].ebp = g_ebp; jbs[slot].esp = g_esp;
  return &jbs[slot].jb;
}
void imp_longjmp(void) {
  uint32_t key = ARG(0), val = ARG(1);
  for (int i = 0; i < MAX_JB; i++) if (jbs[i].key == key) {
    g_ebx = jbs[i].ebx; g_esi = jbs[i].esi; g_edi = jbs[i].edi; g_ebp = jbs[i].ebp; g_esp = jbs[i].esp;
    g_eax = val ? val : 1;
    longjmp(jbs[i].jb, 1);
  }
  fprintf(stderr, "longjmp to unknown jmp_buf %x\n", key); abort();
}

#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
// Debug: scan guest memory for a byte pattern (used to find where/if decoded data landed).
EMSCRIPTEN_KEEPALIVE uint32_t rt_mem_base(void) { return 0; }
#endif
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
// Debug: records only the FIRST call of each guest function in this page session (plus socket markers),
// so code that runs for the first time at a given moment (e.g. opening a tab) stands out in a small buffer.
#define RT_FIRST_N (1u << 16)
static uint8_t rt_seen[1u << 24];
static uint32_t rt_first[RT_FIRST_N];
static volatile uint32_t rt_fcount;
void rt_watch_hit(uint32_t a) {
  uint32_t k = a & ((1u << 24) - 1);
  if (rt_seen[k]) return;
  rt_seen[k] = 1;
  uint32_t i = __atomic_fetch_add(&rt_fcount, 1, __ATOMIC_SEQ_CST);
  if (i < RT_FIRST_N) rt_first[i] = a;
}
EMSCRIPTEN_KEEPALIVE uint32_t *rt_first_base(void) { return rt_first; }
EMSCRIPTEN_KEEPALIVE uint32_t rt_first_count(void) { return rt_fcount; }
#endif
