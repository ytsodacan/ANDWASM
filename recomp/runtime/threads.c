// bionic pthreads on top of Emscripten pthreads + futexes.
// Guest pthread_mutex_t / pthread_cond_t / pthread_once_t are 4-byte words in guest memory; we
// implement them directly on those words so static initialisers in the game's data just work.
#include "rt.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <sys/time.h>
#include <emscripten/threading.h>
#include <stdatomic.h>
#include <errno.h>
#include <unistd.h>
#include <sched.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
extern _Thread_local int g_errno;
static _Thread_local uint32_t self_id = 1;
static uint32_t next_tid = 2;

static inline _Atomic uint32_t *W(uint32_t a) { return (_Atomic uint32_t *)P(a); }
static void fwait(uint32_t a, uint32_t v, double ms) { emscripten_futex_wait((void *)P(a), v, ms); }
static void fwake(uint32_t a, int n) { emscripten_futex_wake((void *)P(a), n); }

// ---------------------------------------------------------------- mutex
// bionic encodes the type in bits 14-15 (0x4000 = recursive, 0x8000 = errorcheck) of the word
// and keeps state in bits 0-1. We keep that layout: state 0 unlocked, 1 locked, 2 locked+waiters.
// Recursive owners/counts live in a side table (bionic packs them into the word; nothing else reads it).
#define MTYPE(v) ((v) & 0xC000u)
#define MAX_REC 1024
static struct { uint32_t addr, owner, count; } rec[MAX_REC];
static pthread_mutex_t rec_lock = PTHREAD_MUTEX_INITIALIZER;
static int rec_find(uint32_t a, int create) {
  for (int i = 0; i < MAX_REC; i++) if (rec[i].addr == a) return i;
  if (!create) return -1;
  for (int i = 0; i < MAX_REC; i++) if (!rec[i].addr) { rec[i].addr = a; return i; }
  LOGI("out of recursive mutex slots\n"); abort();
}

static void raw_lock(uint32_t a) {
  _Atomic uint32_t *w = W(a);
  uint32_t t = MTYPE(atomic_load(w));
  uint32_t expect = t;
  if (atomic_compare_exchange_strong(w, &expect, t | 1)) return;
  for (;;) {
    uint32_t old = atomic_exchange(w, t | 2);
    if ((old & 3) == 0) return;
    fwait(a, t | 2, 50);
  }
}
static void raw_unlock(uint32_t a) {
  _Atomic uint32_t *w = W(a);
  uint32_t t = MTYPE(atomic_load(w));
  if ((atomic_exchange(w, t) & 3) == 2) fwake(a, 1);
}
static int mutex_lock(uint32_t a, int try_only) {
  uint32_t v = atomic_load(W(a));
  if (MTYPE(v) == 0x4000) {
    pthread_mutex_lock(&rec_lock); int i = rec_find(a, 1);
    if (rec[i].owner == self_id) { rec[i].count++; pthread_mutex_unlock(&rec_lock); return 0; }
    pthread_mutex_unlock(&rec_lock);
    if (try_only) { uint32_t e = MTYPE(v); if (!atomic_compare_exchange_strong(W(a), &e, e | 1)) return 16; }
    else raw_lock(a);
    pthread_mutex_lock(&rec_lock); rec[i].owner = self_id; rec[i].count = 1; pthread_mutex_unlock(&rec_lock);
    return 0;
  }
  if (try_only) { uint32_t e = MTYPE(v); return atomic_compare_exchange_strong(W(a), &e, e | 1) ? 0 : 16; }
  raw_lock(a); return 0;
}
static void mutex_unlock(uint32_t a) {
  if (MTYPE(atomic_load(W(a))) == 0x4000) {
    pthread_mutex_lock(&rec_lock); int i = rec_find(a, 0);
    if (i >= 0 && rec[i].owner == self_id && --rec[i].count > 0) { pthread_mutex_unlock(&rec_lock); return; }
    if (i >= 0) rec[i].owner = 0;
    pthread_mutex_unlock(&rec_lock);
  }
  raw_unlock(a);
}
void imp_pthread_mutex_init(void) {
  uint32_t type = ARG(1) ? M32(ARG(1)) : 0;   // our mutexattr is just the type number
  W32(ARG(0), type == 1 ? 0x4000u : type == 2 ? 0x8000u : 0); g_eax = 0;
}
void imp_pthread_mutex_lock(void) { g_eax = mutex_lock(ARG(0), 0); }
void imp_pthread_mutex_trylock(void) { g_eax = mutex_lock(ARG(0), 1); }
void imp_pthread_mutex_unlock(void) { mutex_unlock(ARG(0)); g_eax = 0; }
void imp_pthread_mutex_destroy(void) { g_eax = 0; }
void imp_pthread_mutexattr_init(void) { W32(ARG(0), 0); g_eax = 0; }
void imp_pthread_mutexattr_destroy(void) { g_eax = 0; }
void imp_pthread_mutexattr_settype(void) { W32(ARG(0), ARG(1)); g_eax = 0; }

// ---------------------------------------------------------------- condition variables (sequence counter)
static int cond_wait(uint32_t c, uint32_t m, double ms) {
  uint32_t seq = atomic_load(W(c));
  mutex_unlock(m);
  int r = emscripten_futex_wait((void *)P(c), seq, ms);
  mutex_lock(m, 0);
  return r == -ETIMEDOUT ? 110 : 0;   // bionic ETIMEDOUT
}
void imp_pthread_cond_wait(void) { g_eax = cond_wait(ARG(0), ARG(1), INFINITY); }
void imp_pthread_cond_timedwait(void) {
  uint32_t ts = ARG(2);
  struct timeval now; gettimeofday(&now, NULL);
  double ms = ((double)(int32_t)M32(ts) - now.tv_sec) * 1000.0 + ((double)(int32_t)M32(ts + 4) / 1e6 - now.tv_usec / 1e3);
  g_eax = cond_wait(ARG(0), ARG(1), ms > 0 ? ms : 0);
}
void imp_pthread_cond_signal(void) { atomic_fetch_add(W(ARG(0)), 1); fwake(ARG(0), 1); g_eax = 0; }
void imp_pthread_cond_broadcast(void) { atomic_fetch_add(W(ARG(0)), 1); fwake(ARG(0), 0x7FFFFFFF); g_eax = 0; }
void imp_pthread_cond_destroy(void) { g_eax = 0; }

// ---------------------------------------------------------------- once / keys
void imp_pthread_once(void) {
  // states: 0 new, 1 running, 2 done
  uint32_t a = ARG(0), fn = ARG(1);
  uint32_t e = 0;
  if (atomic_compare_exchange_strong(W(a), &e, 1)) {
    rt_call(fn, 0);
    atomic_store(W(a), 2); fwake(a, 0x7FFFFFFF);
  } else while (atomic_load(W(a)) != 2) fwait(a, 1, 10);
  g_eax = 0;
}
#define MAX_KEYS 128
static _Atomic uint32_t key_next = 1;
static _Thread_local uint32_t key_val[MAX_KEYS];
void imp_pthread_key_create(void) { W32(ARG(0), atomic_fetch_add(&key_next, 1)); g_eax = 0; }
void imp_pthread_key_delete(void) { g_eax = 0; }
void imp_pthread_getspecific(void) { g_eax = ARG(0) < MAX_KEYS ? key_val[ARG(0)] : 0; }
void imp_pthread_setspecific(void) { if (ARG(0) < MAX_KEYS) key_val[ARG(0)] = ARG(1); g_eax = 0; }

// ---------------------------------------------------------------- threads
#define MAX_THREADS 64
static struct { uint32_t id; pthread_t host; uint32_t ret; } threads[MAX_THREADS];
typedef struct { uint32_t entry, arg, id, stack_size; } start_t;
static void *trampoline(void *p) {
  start_t s = *(start_t *)p; free(p);
  self_id = s.id;
  uint32_t stack = (uint32_t)(uintptr_t)malloc(s.stack_size);
  g_esp = (stack + s.stack_size - 64) & ~15u;
  g_gs = (uint32_t)(uintptr_t)calloc(1, 4096);
  W32(g_gs, g_gs); W32(g_gs + 4, s.id); W32(g_gs + 0x14, 0x2B1D5EEDu);   // bionic TLS: self, tid, guard
  if (getenv("THREAD_TRACE")) LOGI("thread #%u running entry=%x esp=%x gs=%x\n", s.id, s.entry - B, g_esp, g_gs);
  { void rt_register_thread(uint32_t); rt_register_thread(s.id); }
  uint32_t r = rt_call(s.entry, 1, s.arg);
  if (getenv("THREAD_TRACE")) LOGI("thread #%u exited -> %x\n", s.id, r);
  for (int i = 0; i < MAX_THREADS; i++) if (threads[i].id == s.id) threads[i].ret = r;
  return NULL;
}
void imp_pthread_create(void) {
  start_t *s = malloc(sizeof *s);
  s->entry = ARG(2); s->arg = ARG(3); s->id = __atomic_fetch_add(&next_tid, 1, __ATOMIC_SEQ_CST);
  s->stack_size = 1u << 20;
  int slot = -1;
  for (int i = 0; i < MAX_THREADS; i++) { uint32_t z = 0; if (__atomic_compare_exchange_n(&threads[i].id, &z, s->id, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) { slot = i; break; } }
  if (slot < 0) { LOGI("too many guest threads\n"); g_eax = 11; return; }
  W32(ARG(0), s->id);
  if (getenv("THREAD_TRACE")) LOGI("pthread_create #%u entry=%x\n", s->id, s->entry - B);
  pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setstacksize(&at, 256 << 10);
  g_eax = pthread_create(&threads[slot].host, &at, trampoline, s);
}
void imp_pthread_join(void) {
  for (int i = 0; i < MAX_THREADS; i++) if (threads[i].id == ARG(0)) {
    pthread_join(threads[i].host, NULL);
    if (ARG(1)) W32(ARG(1), threads[i].ret);
    threads[i].id = 0; g_eax = 0; return;
  }
  g_eax = 3;   // ESRCH
}
void imp_pthread_detach(void) {
  for (int i = 0; i < MAX_THREADS; i++) if (threads[i].id == ARG(0)) pthread_detach(threads[i].host);
  g_eax = 0;
}
void imp_pthread_self(void) { g_eax = self_id; }
void imp_pthread_equal(void) { g_eax = ARG(0) == ARG(1); }
void imp_sched_yield(void) { sched_yield(); g_eax = 0; }
