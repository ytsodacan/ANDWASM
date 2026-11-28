// Runtime interface for lifted ARMv7 / Thumb-2 code (wasm32: guest addresses == wasm addresses).
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

typedef struct { uint8_t type; uint32_t off; uint32_t val; int32_t imp; } reloc_t;
typedef struct { const char *name; uint32_t off; } export_t;

// VFP / NEON register file: s0-s31 overlay d0-d15, q-registers are d pairs.
typedef union { float s[64]; double d[32]; uint32_t w[64]; int32_t i[64]; uint64_t x[32];
                uint16_t h[128]; uint8_t b[256]; } vreg_t;

extern uint32_t B;                        // load base of the image
extern _Thread_local uint32_t g_r[16];    // r0-r12, sp (13), lr (14); per guest thread
extern _Thread_local vreg_t V;
extern _Thread_local uint32_t g_nzcv, g_lr0;
extern _Thread_local uint32_t g_unwind;   // non-zero: a return is passing through lifted frames to this call site     // flags carried into split-function entries

#define SYNC_OUT g_r[0]=r0;g_r[1]=r1;g_r[2]=r2;g_r[3]=r3;g_r[4]=r4;g_r[5]=r5;g_r[6]=r6;g_r[7]=r7;g_r[8]=r8;\
  g_r[9]=r9;g_r[10]=r10;g_r[11]=r11;g_r[12]=r12;g_r[13]=sp;g_r[14]=lr;
#define SYNC_IN  r0=g_r[0];r1=g_r[1];r2=g_r[2];r3=g_r[3];r4=g_r[4];r5=g_r[5];r6=g_r[6];r7=g_r[7];r8=g_r[8];\
  r9=g_r[9];r10=g_r[10];r11=g_r[11];r12=g_r[12];sp=g_r[13];lr=g_r[14];

typedef uint16_t __attribute__((aligned(1), may_alias)) u16u;
typedef uint32_t __attribute__((aligned(1), may_alias)) u32u;
typedef uint64_t __attribute__((aligned(1), may_alias)) u64u;
typedef float    __attribute__((aligned(1), may_alias)) f32u;
typedef double   __attribute__((aligned(1), may_alias)) f64u;
#define P(a) ((uintptr_t)(uint32_t)(a))
#define M8(a)  (*(uint8_t*)P(a))
#define M16(a) (*(u16u*)P(a))
#define M32(a) (*(u32u*)P(a))
#define M64(a) (*(u64u*)P(a))
#define MF32(a) (*(f32u*)P(a))
#define MF64(a) (*(f64u*)P(a))
#define W8(a,v)  (*(uint8_t*)P(a)=(uint8_t)(v))
#define W16(a,v) (*(u16u*)P(a)=(uint16_t)(v))
#define W32(a,v) (*(u32u*)P(a)=(uint32_t)(v))
#define W64(a,v) (*(u64u*)P(a)=(uint64_t)(v))
#define WF32(a,v) (*(f32u*)P(a)=(float)(v))
#define WF64(a,v) (*(f64u*)P(a)=(double)(v))

// Barrel shifter. Amounts come from immediates or the low byte of a register (0..255).
static inline uint32_t LSL(uint32_t x, uint32_t n) { return n >= 32 ? 0 : x << n; }
static inline uint32_t LSR(uint32_t x, uint32_t n) { return n >= 32 ? 0 : x >> n; }
static inline uint32_t ASR(uint32_t x, uint32_t n) { return (uint32_t)((int32_t)x >> (n >= 32 ? 31 : n)); }
static inline uint32_t ROR(uint32_t x, uint32_t n) { n &= 31; return n ? (x >> n) | (x << (32 - n)) : x; }
static inline uint32_t LSLC(uint32_t x, uint32_t n, uint32_t *c) {
  if (!n) return x; if (n < 32) { *c = (x >> (32 - n)) & 1; return x << n; } *c = n == 32 ? (x & 1) : 0; return 0; }
static inline uint32_t LSRC(uint32_t x, uint32_t n, uint32_t *c) {
  if (!n) return x; if (n < 32) { *c = (x >> (n - 1)) & 1; return x >> n; } *c = n == 32 ? (x >> 31) : 0; return 0; }
static inline uint32_t ASRC(uint32_t x, uint32_t n, uint32_t *c) {
  if (!n) return x; if (n < 32) { *c = (x >> (n - 1)) & 1; return (uint32_t)((int32_t)x >> n); }
  *c = x >> 31; return (uint32_t)((int32_t)x >> 31); }
static inline uint32_t RORC(uint32_t x, uint32_t n, uint32_t *c) {
  if (!n) return x; uint32_t r = ROR(x, n); *c = r >> 31; return r; }
static inline uint32_t rbit32(uint32_t x) {
  x = ((x >> 1) & 0x55555555u) | ((x & 0x55555555u) << 1); x = ((x >> 2) & 0x33333333u) | ((x & 0x33333333u) << 2);
  x = ((x >> 4) & 0x0F0F0F0Fu) | ((x & 0x0F0F0F0Fu) << 4); return __builtin_bswap32(x); }
// VCVT to integer: saturating, round toward zero (mode 0) or to nearest (mode 1, VCVTR default FPSCR).
static inline uint32_t cvt_s32(double v, int mode) {
  if (v != v) return 0; if (mode) v = nearbyint(v);
  if (v >= 2147483647.0) return 0x7FFFFFFFu; if (v <= -2147483648.0) return 0x80000000u; return (uint32_t)(int32_t)v; }
static inline uint32_t cvt_u32(double v, int mode) {
  if (v != v || v <= 0) return 0; if (mode) v = nearbyint(v); if (v >= 4294967295.0) return 0xFFFFFFFFu; return (uint32_t)v; }

// LDREX/STREX: the exclusive monitor is emulated with compare-and-swap on the value LDREX saw.
extern _Thread_local uint32_t rt_excl_addr; extern _Thread_local uint64_t rt_excl_val;
static inline void rt_excl_set(uint32_t a, uint64_t v) { rt_excl_addr = a; rt_excl_val = v; }
static inline uint32_t rt_excl_store32(uint32_t a, uint32_t v) {
  if (a != rt_excl_addr) return 1; uint32_t e = (uint32_t)rt_excl_val; rt_excl_addr = 0;
  return __atomic_compare_exchange_n((uint32_t *)P(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; }
static inline uint32_t rt_excl_store16(uint32_t a, uint32_t v) {
  if (a != rt_excl_addr) return 1; uint16_t e = (uint16_t)rt_excl_val; rt_excl_addr = 0;
  return __atomic_compare_exchange_n((uint16_t *)P(a), &e, (uint16_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; }
static inline uint32_t rt_excl_store8(uint32_t a, uint32_t v) {
  if (a != rt_excl_addr) return 1; uint8_t e = (uint8_t)rt_excl_val; rt_excl_addr = 0;
  return __atomic_compare_exchange_n((uint8_t *)P(a), &e, (uint8_t)v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; }
static inline uint32_t rt_excl_store64(uint32_t a, uint64_t v) {
  if (a != rt_excl_addr) return 1; uint64_t e = rt_excl_val; rt_excl_addr = 0;
  return __atomic_compare_exchange_n((uint64_t *)P(a), &e, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST) ? 0 : 1; }

// Call trace: last 256 function entries per thread, dumped on crashes.
extern _Thread_local uint32_t rt_ring[256];
extern _Thread_local uint32_t rt_ring_i;
#ifdef RT_RING      // debug builds: last 256 guest function entries per thread (crash reports, watchdog)
#define RT_TRACE(a) (rt_ring[rt_ring_i++ & 255] = (a))
#else
#define RT_TRACE(a) ((void)0)
#endif

uint32_t rt_tls(void);                    // TPIDRURO: bionic TLS block of the current guest thread
void rt_svc(uint32_t off);
void rt_trace_hit(uint32_t fn);
void rt_dispatch(uint32_t addr);
void rt_tail(uint32_t addr);   // indirect jump: tail call, or a return to a caller's call site
typedef void (*rt_fn)(void);
rt_fn rt_tail_fn(uint32_t addr);   // target of an indirect jump, or NULL when it is a return (g_unwind set)
rt_fn rt_dispatch_fn(uint32_t addr);
// Guaranteed tail calls (wasm return_call): control flowing between lifted functions reuses the frame.
#define MT(call) do { __attribute__((musttail)) return call; } while (0)
#define MT_TAIL(a) do { rt_fn f_ = rt_tail_fn(a); if (f_) { __attribute__((musttail)) return f_(); } return; } while (0)
#define MT_DISPATCH(a) do { rt_fn f_ = rt_dispatch_fn(a); __attribute__((musttail)) return f_(); } while (0)
// Same without musttail (functions that call setjmp)
#define NT(call) do { call; return; } while (0)
#define NT_TAIL(a) do { rt_tail(a); return; } while (0)
#define NT_DISPATCH(a) do { rt_dispatch(a); return; } while (0)
void rt_unsupported(uint32_t off) __attribute__((noreturn));
void rt_fallthrough(uint32_t off) __attribute__((noreturn));
void rt_trap(uint32_t off) __attribute__((noreturn));
void rt_missing_import(const char *name);

// ---- generated tables (table.c) ----
extern const uint32_t IMAGE_SIZE, FUNC_COUNT, RELOC_COUNT, INIT_COUNT, IMPORT_COUNT, EXPORT_COUNT;
extern const char IMAGE_NAME[];
extern const uint32_t FUNC_ADDR[], INIT_ARRAY[];
extern void (*const FUNC_PTR[])(void);
extern const reloc_t RELOCS[];
extern const char *const IMPORT_NAME[];
extern void (*const IMPORT_FN[])(void);
extern const export_t EXPORTS[];

// ---- AAPCS (softfp) argument access for host implementations of imports ----
// Integer/pointer argument words: r0-r3, then the stack. 64-bit values (double, long long) take an
// even-numbered word pair. Floats are passed as their bit patterns in core registers (softfp).
static inline uint32_t ARGW(uint32_t n) { return n < 4 ? g_r[n] : M32(g_r[13] + 4u * (n - 4)); }   // evaluates n once
#define ARG(n) ARGW(n)
#define ARGP(n) ((void *)P(ARG(n)))
#define RET(v) (g_r[0] = (uint32_t)(v))
#define RET64(v) do { uint64_t r_ = (uint64_t)(v); g_r[0] = (uint32_t)r_; g_r[1] = (uint32_t)(r_ >> 32); } while (0)
static inline float  argf_w(int w) { uint32_t u = ARGW(w); float f; memcpy(&f, &u, 4); return f; }
static inline double argd_w(int w) { uint64_t q = (uint64_t)ARGW(w) | ((uint64_t)ARGW(w + 1) << 32); double d; memcpy(&d, &q, 8); return d; }
static inline uint64_t argl_w(int w) { return (uint64_t)ARGW(w) | ((uint64_t)ARGW(w + 1) << 32); }
static inline void retf(float f) { uint32_t u; memcpy(&u, &f, 4); g_r[0] = u; }
static inline void retd(double d) { uint64_t q; memcpy(&q, &d, 8); g_r[0] = (uint32_t)q; g_r[1] = (uint32_t)(q >> 32); }

#define THUNK_STRIDE 16u
uint32_t rt_thunk_base(void);
uint32_t rt_export(const char *name);           // guest address of an exported symbol (Thumb bit kept)
uint32_t rt_call(uint32_t fn, int argc, ...);   // call a guest function (AAPCS, word arguments) from host
uint64_t rt_call64(uint32_t fn, int argc, ...); // same, returns r1:r0
uint32_t rt_host_fn(void (*fn)(void));          // make a host function callable by guest code

// setjmp/longjmp: lifted code calls host setjmp on a slot keyed by the guest jmp_buf address.
jmp_buf *rt_setjmp_slot(uint32_t guest_buf);
