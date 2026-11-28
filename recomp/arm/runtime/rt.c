// Core runtime for lifted ARM code: image load, ARM relocations, dispatch, host<->guest calls, TLS.
#include "rt.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>

uint32_t B;
_Thread_local uint32_t g_r[16];
_Thread_local vreg_t V;
_Thread_local uint32_t g_nzcv, g_lr0;
_Thread_local uint32_t g_unwind;
_Thread_local uint32_t rt_excl_addr;
_Thread_local uint64_t rt_excl_val;
_Thread_local uint32_t rt_ring[256];
_Thread_local uint32_t rt_ring_i;
static _Thread_local uint32_t tls_block;

#define MAX_HOST_FNS 2048
static uint32_t thunk_base, thunk_end;
static void (*host_fns[MAX_HOST_FNS])(void);
static int host_count;

uint32_t rt_thunk_base(void) { return thunk_base; }

// Bionic TLS: slot 0 self, 1 thread id, 2 errno (older bionic), 5 stack guard. One block per thread.
uint32_t rt_tls(void) {
  if (!tls_block) {
    tls_block = (uint32_t)(uintptr_t)calloc(1, 4096);
    W32(tls_block, tls_block); W32(tls_block + 4, 1); W32(tls_block + 20, 0x2B1D5EEDu);
  }
  return tls_block;
}
void rt_set_tls_tid(uint32_t tid) { W32(rt_tls() + 4, tid); }

static void dump_regs(void) {
  fprintf(stderr, "  last calls (oldest->newest):");
  for (uint32_t i = 64; i > 0; i--) fprintf(stderr, " %x", rt_ring[(rt_ring_i - i) & 255]);
  fprintf(stderr, "\n  r0=%08x r1=%08x r2=%08x r3=%08x r4=%08x r5=%08x r6=%08x r7=%08x\n", g_r[0], g_r[1], g_r[2], g_r[3], g_r[4], g_r[5], g_r[6], g_r[7]);
  fprintf(stderr, "  r8=%08x r9=%08x r10=%08x r11=%08x r12=%08x sp=%08x lr=%08x B=%08x (lr-B=%x)\n", g_r[8], g_r[9], g_r[10], g_r[11], g_r[12], g_r[13], g_r[14], B, g_r[14] - B);
  fprintf(stderr, "  stack code addresses (image offsets):");
  for (int i = 0; i < 96; i++) { uint32_t v = M32(g_r[13] + 4 * i); if (v >= B && v < B + IMAGE_SIZE) fprintf(stderr, " %x", v - B); }
  fprintf(stderr, "\n");
}
void rt_dump(void) { dump_regs(); }
void rt_unsupported(uint32_t off) { fprintf(stderr, "unsupported instruction at %x\n", off); dump_regs(); abort(); }
void rt_fallthrough(uint32_t off) { fprintf(stderr, "fell off end of function %x\n", off); dump_regs(); abort(); }
void rt_trap(uint32_t off) { fprintf(stderr, "trap at %x\n", off); dump_regs(); abort(); }
void rt_svc(uint32_t off) {
  if (g_r[7] == 0xF0002u) { g_r[0] = 0; return; }        // __ARM_NR_cacheflush: nothing to flush
  fprintf(stderr, "svc (system call) at %x, r7=%x\n", off, g_r[7]); dump_regs(); abort();
}
void rt_missing_import(const char *name) { fprintf(stderr, "unimplemented import: %s\n", name); dump_regs(); abort(); }
void rt_trace_hit(uint32_t fn) {
  fprintf(stderr, "[trace] F_%X r0=%x r1=%x r2=%x r3=%x lr=%x  recent:", fn, g_r[0], g_r[1], g_r[2], g_r[3], g_r[14] - B);
  for (uint32_t k = 24; k > 1; k--) fprintf(stderr, " %x", rt_ring[(rt_ring_i - k) & 255]);
  fprintf(stderr, "\n");
  for (int k = 0; k < 4; k++) { uint32_t a = g_r[k]; if (a > 0x10000 && (uint64_t)a + 64 < (uint64_t)__builtin_wasm_memory_size(0) * 65536u) { const char *c = (const char *)P(a); int n = 0;
    while (n < 60 && c[n] >= 32 && c[n] < 127) n++; if (n >= 3 && !c[n]) fprintf(stderr, "        r%d=\"%s\"\n", k, c); } }
}

static int find_func(uint32_t off) {
  uint32_t lo = 0, hi = FUNC_COUNT;
  while (lo < hi) { uint32_t mid = (lo + hi) / 2; if (FUNC_ADDR[mid] < off) lo = mid + 1; else hi = mid; }
  return (lo < FUNC_COUNT && FUNC_ADDR[lo] == off) ? (int)lo : -1;
}

void rt_dispatch(uint32_t addr) {
  uint32_t a = addr & ~1u, off = a - B;
  if (off < IMAGE_SIZE) {
    int i = find_func(off);
    if (i >= 0) { FUNC_PTR[i](); return; }
    fprintf(stderr, "dispatch to unknown code offset %x\n", off); dump_regs(); abort();
  }
  if (a >= thunk_base && a < thunk_end) {
    uint32_t idx = (a - thunk_base) / THUNK_STRIDE;
    void (*fn)(void) = idx < IMPORT_COUNT ? IMPORT_FN[idx] : host_fns[idx - IMPORT_COUNT];
    fn();
    return;
  }
  fprintf(stderr, "dispatch to wild address %08x\n", addr); dump_regs(); abort();
}

// An indirect PC write whose target is not a function entry but lies in the image is a return to a
// caller's call site (its lifted C frame is our caller): just return. Real entries are tail calls.
static void missing_fn(void) { fprintf(stderr, "dispatch to unknown code (tail)\n"); dump_regs(); abort(); }
rt_fn rt_dispatch_fn(uint32_t addr) {
  uint32_t a = addr & ~1u, off = a - B;
  if (off < IMAGE_SIZE) {
    int i = find_func(off);
    if (i >= 0) return FUNC_PTR[i];
    fprintf(stderr, "dispatch to unknown code offset %x\n", off); dump_regs(); abort();
  }
  if (a >= thunk_base && a < thunk_end) {
    uint32_t idx = (a - thunk_base) / THUNK_STRIDE;
    return idx < IMPORT_COUNT ? IMPORT_FN[idx] : host_fns[idx - IMPORT_COUNT];
  }
  fprintf(stderr, "dispatch to wild address %08x\n", addr); dump_regs(); abort();
  return missing_fn;
}
rt_fn rt_tail_fn(uint32_t addr) {
  uint32_t off = (addr & ~1u) - B;
  if (off < IMAGE_SIZE && find_func(off) < 0) { g_unwind = addr; return 0; }
  if ((addr & ~1u) == 0xFFFFFFF0u) { g_unwind = addr; return 0; }   // return to a host rt_call (its fake lr)
  return rt_dispatch_fn(addr);
}
void rt_tail(uint32_t addr) {
  uint32_t off = (addr & ~1u) - B;
  if (off < IMAGE_SIZE && find_func(off) < 0) { g_unwind = addr; return; }
  if ((addr & ~1u) == 0xFFFFFFF0u) { g_unwind = addr; return; }
  rt_dispatch(addr);
}

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

// Host -> guest call with word arguments (AAPCS: r0-r3, rest on an 8-byte aligned stack).
// Every register except the results is restored afterwards, so the caller's view is unchanged.
static uint64_t call_v(uint32_t fn, int argc, va_list ap) {
  uint32_t saved[16]; memcpy(saved, g_r, sizeof saved);
  uint32_t args[16]; for (int i = 0; i < argc && i < 16; i++) args[i] = va_arg(ap, uint32_t);
  uint32_t sp = g_r[13];
  int nstack = argc > 4 ? argc - 4 : 0;
  sp -= 4u * nstack; sp &= ~7u;
  for (int i = 0; i < nstack; i++) W32(sp + 4u * i, args[4 + i]);
  for (int i = 0; i < 4; i++) g_r[i] = i < argc ? args[i] : 0;
  g_r[13] = sp;
  g_r[14] = 0xFFFFFFF0u;       // fake return address: the callee's final PC write equals it -> C return
  rt_dispatch(fn);
  g_unwind = 0;                // a return aimed past the host call ends here
  uint64_t r = (uint64_t)g_r[0] | ((uint64_t)g_r[1] << 32);
  memcpy(g_r, saved, sizeof saved);
  return r;
}
uint32_t rt_call(uint32_t fn, int argc, ...) { va_list ap; va_start(ap, argc); uint64_t r = call_v(fn, argc, ap); va_end(ap); return (uint32_t)r; }
uint64_t rt_call64(uint32_t fn, int argc, ...) { va_list ap; va_start(ap, argc); uint64_t r = call_v(fn, argc, ap); va_end(ap); return r; }

// ---- boot ----
extern uint32_t hle_data_import(const char *name);  // guest address of a data import, or 0

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
      case 23: W32(p, M32(p) + B); break;          // R_ARM_RELATIVE
      case 2: W32(p, M32(p) + s); break;           // R_ARM_ABS32
      case 21: case 22: W32(p, s); break;          // GLOB_DAT / JUMP_SLOT
      case 3: W32(p, M32(p) + s - p); break;       // R_ARM_REL32
      case 0: break;
      default: fprintf(stderr, "reloc type %d at %x\n", r->type, r->off); return -1;
    }
  }
  return 0;
}

// Watchdog: a host thread that periodically prints what the main guest thread is running (debug aid
// for hangs, since a busy main thread blocks the page's own console tools).
static uint32_t *main_ring, *main_ring_i;
#ifdef __EMSCRIPTEN__
#include <emscripten/console.h>
#include <emscripten.h>
#endif
static void wlog(const char *m) {
#ifdef __EMSCRIPTEN__
  // In Node, append to a file synchronously from this worker (its console output would wait for the busy main
  // thread); in a browser, log straight from the worker.
  if (!EM_ASM_INT({ if (typeof process === 'object' && process.versions && process.versions.node) {
        try { var fs = (typeof require === 'function') ? require('fs') : null; if (fs) { fs.appendFileSync('/tmp/andwasm_watchdog.log', UTF8ToString($0) + '\n'); return 1; } } catch (e) {} }
        return 0; }, m))
    emscripten_console_log(m);
#else
  fputs(m, stderr); fputc('\n', stderr);
#endif
}
static void *watchdog(void *arg) {
  (void)arg;
  uint32_t last = 0;
  for (;;) {
    sleep(3);
    if (!main_ring) continue;
    uint32_t i = *main_ring_i; char b[600]; int o;
    o = snprintf(b, sizeof b, i == last ? "[watchdog] main thread: no guest calls in 3 s; last:" : "[watchdog] main thread calls/3s=%u last:", i - last);
    for (uint32_t k = 24; k > 0 && o < 560; k--) o += snprintf(b + o, sizeof b - o, " %x", main_ring[(i - k) & 255]);
    wlog(b);
    last = i;
  }
  return NULL;
}
void rt_start_watchdog(void) {
  main_ring = rt_ring; main_ring_i = &rt_ring_i;
#ifndef RT_RING
  return;                      // the watchdog reads the call ring, which only debug builds keep
#endif
  if (getenv("NO_WATCHDOG")) return;
  pthread_t t; pthread_create(&t, NULL, watchdog, NULL); pthread_detach(t);
}

void rt_run_init_array(void) {
  for (uint32_t i = 0; i < INIT_COUNT; i++) {
    if (getenv("INIT_TRACE") || i % 16 == 0) fprintf(stderr, "[init] %u/%u %x\n", i, INIT_COUNT, INIT_ARRAY[i]);
    rt_call(B + INIT_ARRAY[i], 0);
  }
}

// ---- setjmp / longjmp ----
// The lifted call site has done SYNC_OUT with r0 = guest jmp_buf; callee-saved registers are recorded
// and the host setjmp runs in the lifted function's own C frame (it returns twice there).
#define MAX_JB 256
static _Thread_local struct { uint32_t key; jmp_buf jb; uint32_t r[16]; vreg_t v; } jbs[MAX_JB];
static _Thread_local uint32_t jb_next;
jmp_buf *rt_setjmp_slot(uint32_t key) {
  int i, slot = -1;
  for (i = 0; i < MAX_JB; i++) { if (jbs[i].key == key) { slot = i; break; } if (!jbs[i].key && slot < 0) slot = i; }
  if (slot < 0) slot = jb_next++ % MAX_JB;
  jbs[slot].key = key;
  memcpy(jbs[slot].r, g_r, sizeof g_r);
  for (int k = 16; k < 32; k++) jbs[slot].v.x[k / 2] = V.x[k / 2];   // d8-d15 are callee-saved
  return &jbs[slot].jb;
}
void imp_longjmp(void) {
  uint32_t key = ARG(0), val = ARG(1);
  for (int i = 0; i < MAX_JB; i++) if (jbs[i].key == key) {
    memcpy(g_r, jbs[i].r, sizeof g_r);
    for (int k = 8; k < 16; k++) V.x[k] = jbs[i].v.x[k];
    g_r[0] = val ? val : 1;
    longjmp(jbs[i].jb, 1);
  }
  fprintf(stderr, "longjmp to unknown jmp_buf %x\n", key); abort();
}
void imp_siglongjmp(void) { imp_longjmp(); }
void imp__longjmp(void) { imp_longjmp(); }
