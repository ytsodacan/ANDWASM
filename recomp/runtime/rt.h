// Runtime interface for lifted i386 code (wasm32: guest addresses == wasm addresses).
#pragma once
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <setjmp.h>

typedef union { float f[4]; double d[2]; uint32_t u[4]; int32_t i[4]; uint64_t q[2];
                uint16_t w[8]; uint8_t b[16]; } xmm_t;
typedef struct { uint8_t type; uint32_t off; uint32_t val; int32_t imp; } reloc_t;
typedef struct { const char *name; uint32_t off; } export_t;

extern uint32_t B;                  // load base of the image
// Guest CPU state is per host thread (each guest pthread runs on its own Emscripten pthread).
extern _Thread_local uint32_t g_eax, g_ecx, g_edx, g_ebx, g_esp, g_ebp, g_esi, g_edi;
extern _Thread_local uint32_t g_gs, g_fs;   // segment bases (bionic TLS lives at gs:0)
extern _Thread_local xmm_t X[8];
extern const xmm_t XZERO;
extern _Thread_local double FST[8];
extern _Thread_local int FTOP;

#define SYNC_OUT g_eax=eax;g_ecx=ecx;g_edx=edx;g_ebx=ebx;g_esp=esp;g_ebp=ebp;g_esi=esi;g_edi=edi;
#define SYNC_IN  eax=g_eax;ecx=g_ecx;edx=g_edx;ebx=g_ebx;esp=g_esp;ebp=g_ebp;esi=g_esi;edi=g_edi;

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
#define PARITY(r) (!__builtin_parity((uint32_t)(r)&0xFFu))

#define ST(i) FST[(FTOP+(i))&7]
static inline void fpush(double v){ FTOP=(FTOP-1)&7; FST[FTOP]=v; }
static inline void fpop(void){ FTOP=(FTOP+1)&7; }
double f80_load(uint32_t a);
void f80_store(uint32_t a, double v);

static inline xmm_t *xload(uint32_t a, int n){
  static _Thread_local xmm_t tmp[4]; static _Thread_local int k; xmm_t *t=&tmp[k++&3];
  *t=XZERO; memcpy(t,(void*)P(a),n); return t; }
static inline void xstore(uint32_t a, const xmm_t *x, int n){ memcpy((void*)P(a),x,n); }
static inline uint32_t cvtt_i32(double v){
  if(!(v>=-2147483648.0 && v<2147483648.0)) return 0x80000000u; return (uint32_t)(int32_t)v; }
static inline int cmp_ps(float a,float b,int p){
  switch(p&7){case 0:return a==b;case 1:return a<b;case 2:return a<=b;case 3:return a!=a||b!=b;
  case 4:return !(a==b);case 5:return !(a<b);case 6:return !(a<=b);default:return a==a&&b==b;} }

// Call trace: last 256 function entries per thread, dumped on crashes.
extern _Thread_local uint32_t rt_ring[256];
extern _Thread_local uint32_t rt_ring_i;
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
void rt_watch_hit(uint32_t a);   // debug: records every guest function entry while Module.__rtWatch is set
#define RT_TRACE(a) (rt_ring[rt_ring_i++ & 255] = (a), rt_watch_hit(a))
#else
#define RT_TRACE(a) (rt_ring[rt_ring_i++ & 255] = (a))
#endif

void rt_trace_hit(uint32_t fn);
void rt_trace_ret(uint32_t fn);
void rt_dispatch(uint32_t addr);
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

// ---- helpers for host import implementations ----
#define ARG(n) M32(g_esp+4+4*(n))
#define ARGP(n) ((void*)P(ARG(n)))
#define THUNK_STRIDE 16u
uint32_t rt_thunk_base(void);
uint32_t rt_export(const char *name);           // guest address of an exported symbol
uint32_t rt_call(uint32_t fn, int argc, ...);   // call guest function (cdecl) from host
uint32_t rt_host_fn(void (*fn)(void));          // make a host function callable by guest code

// setjmp/longjmp: lifted code calls host setjmp on a slot keyed by the guest jmp_buf address.
jmp_buf *rt_setjmp_slot(uint32_t guest_buf);
