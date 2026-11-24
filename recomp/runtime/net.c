// BSD sockets for the guest, carried over WebSockets to bridge/ws_tcp_bridge.py.
// - getaddrinfo hands out fake 10.77.0.N addresses and remembers which hostname each stands for;
//   the bridge (not the browser) decides where a hostname really goes.
// - Each guest TCP socket is one WebSocket owned by the browser main thread. Incoming bytes land in
//   a ring buffer in shared wasm memory; the guest thread waits on it with futexes.
#include "rt.h"
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
void rt_watch_hit(uint32_t a);
#endif
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/select.h>
#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <pthread.h>
#include <emscripten.h>
#include <emscripten/threading.h>

#define LOGI(...) fprintf(stderr, __VA_ARGS__)
extern _Thread_local int g_errno;
int net_trace;

// ---------------------------------------------------------------- fake DNS
#define MAX_HOSTS 32
static char *hosts[MAX_HOSTS]; static int nhosts;
static pthread_mutex_t dns_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t host_ip(const char *h) {   // network order 10.77.0.(i+1)
  pthread_mutex_lock(&dns_lock);
  int i = 0;
  for (; i < nhosts; i++) if (!strcmp(hosts[i], h)) break;
  if (i == nhosts && nhosts < MAX_HOSTS) hosts[nhosts++] = strdup(h);
  pthread_mutex_unlock(&dns_lock);
  return 10u | (77u << 8) | ((uint32_t)(i + 1) << 24);
}
static const char *ip_host(uint32_t ip, char *buf) {
  if ((ip & 0xFFFF) == (10u | (77u << 8)) && (ip >> 24) >= 1 && (int)(ip >> 24) <= nhosts) return hosts[(ip >> 24) - 1];
  snprintf(buf, 32, "%u.%u.%u.%u", ip & 255, (ip >> 8) & 255, (ip >> 16) & 255, ip >> 24);
  return buf;
}

void imp_getaddrinfo(void) {
  const char *node = ARG(0) ? (const char *)P(ARG(0)) : "localhost";
  const char *svc = ARG(1) ? (const char *)P(ARG(1)) : "0";
  uint16_t port = (uint16_t)atoi(svc);
  // bionic struct addrinfo (32 bytes) + sockaddr_in (16 bytes), freed by freeaddrinfo
  uint8_t *m = calloc(1, 48); uint32_t ai = (uint32_t)(uintptr_t)m, sa = ai + 32;
  W32(ai + 4, 2); W32(ai + 8, 1); W32(ai + 12, 6); W32(ai + 16, 16); W32(ai + 24, sa);   // AF_INET, STREAM, TCP
  W16(sa, 2); W16(sa + 2, (uint16_t)((port >> 8) | (port << 8))); W32(sa + 4, host_ip(node));
  W32(ARG(3), ai);
  if (net_trace) LOGI("getaddrinfo(%s, %s) -> fake address\n", node, svc);
  g_eax = 0;
}
void imp_freeaddrinfo(void) { free(ARGP(0)); }
void imp_gai_strerror(void) { static char m[] = "name resolution failed"; g_eax = (uint32_t)(uintptr_t)m; }

// ---------------------------------------------------------------- sockets
#define SOCK_BASE 1000
#define MAX_SOCKS 32
#define RING (1u << 20)
enum { S_FREE, S_CONNECTING, S_OPEN, S_CLOSED, S_FAILED };
typedef struct {
  _Atomic int32_t state;        // written by JS with Atomics.store
  _Atomic uint32_t head;        // bytes written by JS (monotonic)
  uint32_t tail;                // bytes consumed by the guest (monotonic)
  int nonblock, used;
  uint8_t buf[RING];
} sock_t;
static sock_t *socks[MAX_SOCKS];
_Atomic int32_t net_events;     // bumped by JS on any socket event, for select() waits

static sock_t *S(uint32_t fd) {
  return fd >= SOCK_BASE && fd < SOCK_BASE + MAX_SOCKS && socks[fd - SOCK_BASE] && socks[fd - SOCK_BASE]->used ? socks[fd - SOCK_BASE] : NULL;
}
int net_is_sock(uint32_t fd) { return S(fd) != NULL; }

// ---- main-thread JavaScript (the browser main thread owns every WebSocket)
static void js_open(int id, int sock, int ring, const char *host, int port, int evp) {
  MAIN_THREAD_EM_ASM({
    var id = $0; var sock = $1; var ring = $2; var host = UTF8ToString($3); var port = $4; var evp = $5;
    var base = Module.bridgeUrl || ((typeof location !== 'undefined' && location.protocol === 'https:') ? 'wss://' + location.host + '/bridge' : 'ws://127.0.0.1:8643');
    var st = sock >> 2; var hd = (sock + 4) >> 2;
    var bump = function () { Atomics.add(HEAP32, evp >> 2, 1); Atomics.notify(HEAP32, evp >> 2); };
    var set = function (v) { Atomics.store(HEAP32, st, v); Atomics.notify(HEAP32, st); bump(); };
    var open = function (ticket) {
      var ws = new WebSocket(base + '/connect?host=' + encodeURIComponent(host) + '&port=' + port + (ticket ? '&ticket=' + encodeURIComponent(ticket) : '')); ws.binaryType = 'arraybuffer';
      Module.__ws = Module.__ws || {}; Module.__ws[id] = ws;
      ws.onopen = function () { set(2); };
      ws.onerror = function () { if (Atomics.load(HEAP32, st) === 1) set(4); };
      ws.onclose = function () { set(Atomics.load(HEAP32, st) === 1 ? 4 : 3); if (Module.__ws[id] === ws) delete Module.__ws[id]; };
      ws.onmessage = function (e) {
        var d = new Uint8Array(e.data); var h = Atomics.load(HEAP32, hd) >>> 0; var R = 1048576;
        if (Module.__rtWatch) { var L = window.__rtLog = window.__rtLog || []; if (L.length >= 300000) L.splice(0, 100000);
          var hx = ''; for (var q = 0; q < Math.min(16, d.length); q++) hx += ('0' + d[q].toString(16)).slice(-2);
          L.push('WS ' + d.length + ' ' + hx); }
        for (var i = 0; i < d.length; i++) HEAPU8[ring + ((h + i) & (R - 1))] = d[i];
        Atomics.store(HEAP32, hd, (h + d.length) | 0); Atomics.notify(HEAP32, hd); bump();
      };
    };
    // SillyOS login: every connection needs a fresh single-use ticket from the bridge (index.html).
    if (Module.fetchTicket) Module.fetchTicket().then(open).catch(function () { set(4); });
    else open('');
  }, id, sock, ring, host, port, evp);
}
static void js_send(int id, int p, int n) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var ws = Module.__ws && Module.__ws[$0];
    if (ws && ws.readyState === 1) ws.send(HEAPU8.slice($1, $1 + $2));
    _free($1);
  }, id, p, n);
}
static void js_close(int id) {
  MAIN_THREAD_ASYNC_EM_ASM({ var ws = Module.__ws && Module.__ws[$0]; if (ws) ws.close(); }, id);
}

// Fake AF_NETLINK socket: the game enumerates interfaces with RTM_GETADDR; we answer every request
// with an empty multipart dump (a lone NLMSG_DONE), i.e. "no interfaces".
#define NL_FD 999
static uint32_t nl_seq, nl_pending;
static int nl_send(uint32_t fd, uint32_t buf, uint32_t n) {
  if (fd != NL_FD) return -1;
  nl_seq = n >= 12 ? M32(buf + 8) : 0; nl_pending = 1; return (int)n;
}
static int nl_recv(uint32_t fd, uint32_t buf, uint32_t n) {
  if (fd != NL_FD) return -1;
  if (!nl_pending || n < 20) { g_errno = 11; return -2; }
  nl_pending = 0;
  W32(buf, 20); W16(buf + 4, 3 /* NLMSG_DONE */); W16(buf + 6, 2 /* NLM_F_MULTI */);
  W32(buf + 8, nl_seq); W32(buf + 12, 0); W32(buf + 16, 0);
  return 20;
}

void imp_socket(void) {
  if (net_trace) LOGI("socket(%u, %u, %u)\n", ARG(0), ARG(1), ARG(2));
  if (ARG(0) == 16) { g_eax = NL_FD; return; }   // AF_NETLINK
  if (ARG(0) != 2 || (ARG(1) & 0xF) != 1) {   // only AF_INET / SOCK_STREAM
    if (net_trace) LOGI("socket(%u,%u) unsupported\n", ARG(0), ARG(1));
    g_errno = 97; g_eax = (uint32_t)-1; return;   // EAFNOSUPPORT
  }
  for (int i = 0; i < MAX_SOCKS; i++) {
    if (!socks[i]) socks[i] = calloc(1, sizeof(sock_t));   // malloc alignment is enough for the atomics
    if (!socks[i]->used) {
      memset(socks[i], 0, offsetof(sock_t, buf)); socks[i]->used = 1;
      socks[i]->nonblock = (ARG(1) & 04000) != 0;
      g_eax = SOCK_BASE + i; return;
    }
  }
  g_errno = 24; g_eax = (uint32_t)-1;   // EMFILE
}

void imp_connect(void) {
  if (net_trace) LOGI("connect(fd %u, sa %x, len %u)\n", ARG(0), ARG(1), ARG(2));
  sock_t *s = S(ARG(0)); if (!s) { g_errno = 88; g_eax = (uint32_t)-1; return; }
  uint32_t sa = ARG(1);
  uint16_t port = (uint16_t)((M8(sa + 2) << 8) | M8(sa + 3));
  char buf[32]; const char *host = ip_host(M32(sa + 4), buf);
  if (net_trace) LOGI("connect(fd %u -> %s:%u) via WebSocket bridge\n", ARG(0), host, port);
  atomic_store(&s->state, S_CONNECTING);
  js_open((int)(ARG(0) - SOCK_BASE), (int)(uintptr_t)s, (int)(uintptr_t)s->buf, host, (int)port, (int)(uintptr_t)&net_events);
  if (s->nonblock) { g_errno = 115; g_eax = (uint32_t)-1; return; }   // EINPROGRESS
  while (atomic_load(&s->state) == S_CONNECTING) emscripten_futex_wait((void *)&s->state, S_CONNECTING, 100);
  if (net_trace) LOGI("connect(fd %u) -> state %d\n", ARG(0), atomic_load(&s->state));
  if (atomic_load(&s->state) != S_OPEN) { g_errno = 111; g_eax = (uint32_t)-1; return; }   // ECONNREFUSED
  g_eax = 0;
}

void imp_send(void) {
  sock_t *s = S(ARG(0));
  { int r = nl_send(ARG(0), ARG(1), ARG(2)); if (r >= 0) { g_eax = r; return; } }
  if (!s) { g_eax = (uint32_t)write(ARG(0), ARGP(1), ARG(2)); if (net_trace) LOGI("send(pipe fd %u, %u) -> %d errno %d\n", ARG(0), ARG(2), (int)g_eax, errno); return; }   // wake-up pipes
  int st = atomic_load(&s->state);
  if (st == S_CONNECTING) { g_errno = 11; g_eax = (uint32_t)-1; return; }
  if (st != S_OPEN) { g_errno = 32; g_eax = (uint32_t)-1; return; }    // EPIPE
  uint32_t n = ARG(2); uint8_t *copy = malloc(n ? n : 1); memcpy(copy, ARGP(1), n);
  if (net_trace) LOGI("send(fd %u, %u bytes) msg %u len %u\n", ARG(0), n, n >= 5 ? copy[0] << 8 | copy[1] : 0, n >= 5 ? copy[2] << 16 | copy[3] << 8 | copy[4] : 0);
  js_send((int)(ARG(0) - SOCK_BASE), (int)(uintptr_t)copy, (int)n);
  g_eax = n;
}
void imp_sendto(void) { imp_send(); }

void imp_recv(void) {
  sock_t *s = S(ARG(0));
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
  rt_watch_hit(0xE1000000u | (ARG(2) & 0xffff));   // debug: every socket read request (size asked)
#endif
  { int r = nl_recv(ARG(0), ARG(1), ARG(2)); if (r != -1) { g_eax = r < 0 ? (uint32_t)-1 : (uint32_t)r; return; } }
  if (!s) { g_eax = (uint32_t)read(ARG(0), ARGP(1), ARG(2)); if (net_trace) LOGI("recv(pipe fd %u) -> %d\n", ARG(0), (int)g_eax); if ((int32_t)g_eax < 0) g_errno = 11; return; }
  for (;;) {
    uint32_t h = atomic_load(&s->head), avail = h - s->tail;
    if (avail) {
      uint32_t n = avail < ARG(2) ? avail : ARG(2);
      for (uint32_t i = 0; i < n; i++) W8(ARG(1) + i, s->buf[(s->tail + i) & (RING - 1)]);
      s->tail += n; g_eax = n;
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
      rt_watch_hit(0xE2000000u | (n & 0xffff));   // debug: bytes actually delivered
#endif
#if defined(__EMSCRIPTEN__) && defined(RT_WATCH)
      // Debug watch: record every chunk the game reads from its socket (first 16 bytes, hex), while Module.__rtWatch is set.
      if (EM_ASM_INT({ return Module.__rtWatch ? 1 : 0; })) { char hx[64]; int k = n < 16 ? n : 16; for (int j = 0; j < k; j++) sprintf(hx + 2 * j, "%02x", M8(ARG(1) + j)); hx[2 * k] = 0;
        EM_ASM({ var L = window.__rtLog = window.__rtLog || []; if (L.length >= 300000) L.splice(0, 100000); L.push('RECV ' + $0 + ' ' + UTF8ToString($1)); }, n, hx); }
#endif
      if (net_trace) LOGI("recv(fd %u) -> %u bytes%s %u\n", ARG(0), n, n == 7 ? " hdr msg" : "", n == 7 ? M8(ARG(1)) << 8 | M8(ARG(1) + 1) : 0);
      return;
    }
    int st = atomic_load(&s->state);
    if (st == S_CLOSED || st == S_FAILED) { g_eax = 0; return; }
    if (s->nonblock) { g_errno = 11; g_eax = (uint32_t)-1; return; }   // EAGAIN
    emscripten_futex_wait((void *)&s->head, h, 100);
  }
}
void imp_recvfrom(void) { imp_recv(); }

int net_close(uint32_t fd) {
  if (fd == NL_FD) { nl_pending = 0; return 0; }
  sock_t *s = S(fd); if (!s) return -1;
  if (net_trace) LOGI("close(fd %u)\n", fd);
  js_close((int)(fd - SOCK_BASE));
  s->used = 0; return 0;
}
int net_fcntl(uint32_t fd, uint32_t cmd, uint32_t arg) {
  sock_t *s = S(fd); if (!s) return -2;
  if (cmd == 3) return 2 | (s->nonblock ? 04000 : 0);            // F_GETFL -> O_RDWR|O_NONBLOCK
  if (cmd == 4) { s->nonblock = (arg & 04000) != 0; return 0; }   // F_SETFL
  return 0;
}
void imp_setsockopt(void) { g_eax = 0; }
void imp_bind(void) { g_eax = 0; }
void imp_getsockname(void) {
  uint32_t sa = ARG(1); memset((void *)P(sa), 0, 16); W16(sa, 2); W32(sa + 4, 0x0100007Fu);
  if (ARG(2)) W32(ARG(2), 16); g_eax = 0;
}

// select(): sockets from our table, everything else (wake-up pipes) through the host.
static int fdisset(uint32_t set, uint32_t fd) { return set && (M32(set + 4 * (fd / 32)) >> (fd % 32) & 1); }
static void fdset(uint32_t set, uint32_t fd) { W32(set + 4 * (fd / 32), M32(set + 4 * (fd / 32)) | (1u << (fd % 32))); }
void imp_select(void) {
  uint32_t n = ARG(0), rs = ARG(1), ws = ARG(2), es = ARG(3), tv = ARG(4);
  uint32_t words = (n + 31) / 32;
  uint32_t in_r[48] = {0}, in_w[48] = {0};
  if (words > 48) words = 48;
  for (uint32_t w = 0; w < words; w++) { in_r[w] = rs ? M32(rs + 4 * w) : 0; in_w[w] = ws ? M32(ws + 4 * w) : 0; }
  double budget = tv ? (M32(tv) * 1000.0 + M32(tv + 4) / 1000.0) : 1e12;
  double t0 = emscripten_get_now();
  if (net_trace > 1) { static int cnt; if (cnt++ < 20) { LOGI("select enter n=%u tv=%.0fms r:", n, budget);
    for (uint32_t fd = 0; fd < n && fd < words * 32; fd++) { if ((in_r[fd/32] >> (fd%32)) & 1) LOGI(" %u", fd); }
    LOGI(" w:"); for (uint32_t fd = 0; fd < n && fd < words * 32; fd++) { if ((in_w[fd/32] >> (fd%32)) & 1) LOGI(" %u", fd); } LOGI("\n"); } }
  for (;;) {
    int32_t ev = atomic_load(&net_events);
    for (uint32_t w = 0; w < words; w++) { if (rs) W32(rs + 4 * w, 0); if (ws) W32(ws + 4 * w, 0); if (es) W32(es + 4 * w, 0); }
    int ready = 0;
    fd_set hr; FD_ZERO(&hr); int hmax = -1;
    for (uint32_t fd = 0; fd < n && fd < words * 32; fd++) {
      int wr = (in_r[fd / 32] >> (fd % 32)) & 1, ww = (in_w[fd / 32] >> (fd % 32)) & 1;
      if (!wr && !ww) continue;
      sock_t *s = S(fd);
      if (s) {
        int st = atomic_load(&s->state);
        if (wr && (atomic_load(&s->head) != s->tail || st == S_CLOSED || st == S_FAILED)) { fdset(rs, fd); ready++; }
        if (ww && st != S_CONNECTING) { fdset(ws, fd); ready++; }
      } else if (wr && fd < FD_SETSIZE) { FD_SET(fd, &hr); if ((int)fd > hmax) hmax = fd; }
    }
    if (hmax >= 0) {
      struct timeval z = {0, 0};
      if (select(hmax + 1, &hr, NULL, NULL, &z) > 0)
        for (int fd = 0; fd <= hmax; fd++) if (FD_ISSET(fd, &hr)) { fdset(rs, fd); ready++; }
    }
    double left = budget - (emscripten_get_now() - t0);
    if (ready || left <= 0) {
      if (net_trace > 1) LOGI("select(n=%u r0=%x w0=%x r31=%x w31=%x tv=%.0fms) -> %d\n", n, in_r[0], in_w[0], in_r[31], in_w[31], budget, ready);
      g_eax = ready; return; }
    emscripten_futex_wait((void *)&net_events, ev, left < 10 ? left : 10);   // pipes are polled every 10 ms
  }
}
