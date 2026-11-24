// FMOD on Web Audio (browser) / silent FMOD (Node, no AudioContext).
// i386 C++ member functions are cdecl with `this` as the first stack argument; every call returns
// FMOD_OK (0). Handles are fake guest addresses 0x50000000 + 64*id; the JS side keeps the real
// objects in Module.__fm (sounds: decoded AudioBuffers, channels: source+gain, groups: gain nodes).
#include "rt.h"
#include <emscripten.h>
#include <stdio.h>

#define OK g_eax = 0
#define HBASE 0x50000000u
#define MAXH 8192
enum { H_NONE, H_SYSTEM, H_GROUP, H_SOUND, H_CHANNEL };
static uint8_t kind[MAXH];
static uint32_t udata[MAXH];
static uint32_t next_id = 1;
static uint32_t new_handle(int k) {
  uint32_t id = __atomic_fetch_add(&next_id, 1, __ATOMIC_SEQ_CST);
  if (id >= MAXH) { id = 1 + (id % (MAXH - 1)); }   // recycle (old channels are long finished)
  kind[id] = k; udata[id] = 0;
  return HBASE + id * 64;
}
static uint32_t hid(uint32_t h) { uint32_t id = (h - HBASE) / 64; return h >= HBASE && id < MAXH ? id : 0; }

// JS side. All of it runs on the browser main thread (MAIN_THREAD_*), which owns the AudioContext.
static void js_init(void) {
  MAIN_THREAD_EM_ASM({
    if (typeof AudioContext === 'undefined' || Module.__fm) return;
    var ctx = new AudioContext(); var master = ctx.createGain(); master.connect(ctx.destination);
    Module.__actx = ctx;
    var fm = {}; fm.ctx = ctx; fm.master = master; fm.snd = {}; fm.ch = {}; fm.grp = {}; Module.__fm = fm;
  });
}

void imp_FMOD_Debug_Initialize(void) { OK; }
void imp_FMOD_System_Create(void) { W32(ARG(0), new_handle(H_SYSTEM)); js_init(); OK; }
void imp__ZN4FMOD6System4initEijPv(void) { js_init(); OK; }
void imp__ZN4FMOD6System5closeEv(void) { OK; }
void imp__ZN4FMOD6System7releaseEv(void) { OK; }
void imp__ZN4FMOD6System6updateEv(void) { OK; }
void imp__ZN4FMOD6System11mixerResumeEv(void) { MAIN_THREAD_ASYNC_EM_ASM({ var f = Module.__fm; if (f) f.ctx.resume(); }); OK; }
void imp__ZN4FMOD6System12mixerSuspendEv(void) { MAIN_THREAD_ASYNC_EM_ASM({ var f = Module.__fm; if (f) f.ctx.suspend(); }); OK; }
void imp__ZN4FMOD6System13getNumDriversEPi(void) { W32(ARG(1), 1); OK; }
void imp__ZN4FMOD6System13setFileSystemEPF11FMOD_RESULTPKcPjPPvS5_EPFS1_S5_S5_EPFS1_S5_S5_jS4_S5_EPFS1_S5_jS5_EPFS1_P18FMOD_ASYNCREADINFOS5_ESI_i(void) { OK; }
void imp__ZN4FMOD6System16setDSPBufferSizeEji(void) { OK; }
void imp__ZN4FMOD6System19setStreamBufferSizeEjj(void) { OK; }
void imp__ZN4FMOD6System9setOutputE15FMOD_OUTPUTTYPE(void) { OK; }

void imp__ZN4FMOD6System18createChannelGroupEPKcPPNS_12ChannelGroupE(void) {
  uint32_t h = new_handle(H_GROUP); W32(ARG(2), h);
  MAIN_THREAD_EM_ASM({
    var f = Module.__fm; if (!f) return;
    var g = f.ctx.createGain(); g.connect(f.master); var o = {}; o.gain = g; o.paused = false; f.grp[$0] = o;
  }, hid(h));
  OK;
}

// createSound(this, name_or_data, mode, exinfo, Sound**). FMOD_LOOP_NORMAL = 0x2.
static void create_sound(void) {
  uint32_t h = new_handle(H_SOUND); W32(ARG(4), h);
  const char *name = ARG(1) ? (const char *)P(ARG(1)) : "";
  MAIN_THREAD_EM_ASM({
    var f = Module.__fm; if (!f) return;
    var name = UTF8ToString($1); var path = name.charAt(0) === '/' ? '/fs' + name : '/assets/' + name;
    var s = {}; s.buf = null; s.loop = ($2 & 2) !== 0; s.waiters = []; s.ms = 0;
    f.snd[$0] = s;
    var bytes; try { bytes = FS.readFile(path); } catch (e) { console.warn('FMOD: missing ' + path); return; }
    f.ctx.decodeAudioData(bytes.slice().buffer).then(function (b) {
      s.buf = b; s.ms = Math.round(b.duration * 1000); var w = s.waiters; s.waiters = []; for (var i = 0; i < w.length; i++) w[i]();
    }, function () {
      var b = null; try { b = Module.decodeImaWav ? Module.decodeImaWav(f.ctx, bytes) : null; } catch (e) {}
      if (!b) { console.warn('FMOD: cannot decode ' + path); return; }
      s.buf = b; s.ms = Math.round(b.duration * 1000); var w = s.waiters; s.waiters = []; for (var i = 0; i < w.length; i++) w[i]();
    });
  }, hid(h), name, ARG(2));
  OK;
}
void imp__ZN4FMOD6System11createSoundEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE(void) { create_sound(); }
void imp__ZN4FMOD6System12createStreamEPKcjP22FMOD_CREATESOUNDEXINFOPPNS_5SoundE(void) { create_sound(); }
void imp__ZN4FMOD5Sound7releaseEv(void) { MAIN_THREAD_ASYNC_EM_ASM({ var f = Module.__fm; if (f) delete f.snd[$0]; }, hid(ARG(0))); OK; }
void imp__ZN4FMOD5Sound9getLengthEPjj(void) {
  int ms = MAIN_THREAD_EM_ASM_INT({ var f = Module.__fm; var s = f && f.snd[$0]; return s && s.ms ? s.ms : 1000; }, hid(ARG(0)));
  W32(ARG(1), ARG(2) == 1 ? (uint32_t)ms : (uint32_t)ms * 44);   // FMOD_TIMEUNIT_MS = 1, else PCM-ish
  OK;
}

// playSound(this, Sound*, ChannelGroup*, paused, Channel**)
void imp__ZN4FMOD6System9playSoundEPNS_5SoundEPNS_12ChannelGroupEbPPNS_7ChannelE(void) {
  uint32_t h = new_handle(H_CHANNEL); if (ARG(4)) W32(ARG(4), h);
  MAIN_THREAD_EM_ASM({
    var f = Module.__fm; if (!f) return;
    var s = f.snd[$1]; var g = f.grp[$2];
    var gain = f.ctx.createGain(); gain.connect(g ? g.gain : f.master);
    var c = {}; c.snd = s; c.gain = gain; c.src = null; c.paused = !!$3; c.done = false; c.loop = s ? s.loop : false; c.rate = 1; c.offset = 0; c.started = 0; c.group = $2;
    f.ch[$0] = c;
    c.start = function () {
      if (c.done || c.paused || c.src || !c.snd) return;
      if (!c.snd.buf) { c.snd.waiters.push(c.start); return; }
      var src = f.ctx.createBufferSource(); src.buffer = c.snd.buf; src.loop = c.loop; src.playbackRate.value = c.rate;
      src.connect(c.gain); src.onended = function () { if (c.src === src && !c.paused) { c.done = true; c.src = null; } };
      src.start(0, c.offset % c.snd.buf.duration); c.src = src; c.started = f.ctx.currentTime;
    };
    c.halt = function () { if (c.src) { var s = c.src; c.src = null; c.offset += (f.ctx.currentTime - c.started) * c.rate; try { s.stop(); } catch (e) {} } };
    if (g && g.paused) c.paused = true;
    c.start();
  }, hid(h), hid(ARG(1)), hid(ARG(2)), ARG(3) & 0xFF);
  OK;
}

// ChannelControl methods are shared by channels and channel groups.
void imp__ZN4FMOD14ChannelControl11getUserDataEPPv(void) { W32(ARG(1), udata[hid(ARG(0))]); OK; }
void imp__ZN4FMOD14ChannelControl11setUserDataEPv(void) { udata[hid(ARG(0))] = ARG(1); OK; }
void imp__ZN4FMOD14ChannelControl4stopEv(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; if (!f) return;
    var stop = function (c) { c.halt(); c.done = true; };
    if (f.ch[$0]) stop(f.ch[$0]);
    else if (f.grp[$0]) for (var k in f.ch) if (f.ch[k].group === $0) stop(f.ch[k]);
  }, hid(ARG(0)));
  OK;
}
void imp__ZN4FMOD14ChannelControl9isPlayingEPb(void) {
  int r = MAIN_THREAD_EM_ASM_INT({
    var f = Module.__fm; if (!f) return 0; var c = f.ch[$0]; if (!c) return f.grp[$0] ? 1 : 0;
    return c.done ? 0 : 1;   // paused or still decoding counts as playing, like FMOD
  }, hid(ARG(0)));
  W8(ARG(1), r); OK;
}
void imp__ZN4FMOD14ChannelControl9setPausedEb(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; if (!f) return; var p = !!$1;
    var apply = function (c) { if (p) { c.halt(); c.paused = true; } else { c.paused = false; c.start(); } };
    if (f.ch[$0]) apply(f.ch[$0]);
    else if (f.grp[$0]) { f.grp[$0].paused = p; for (var k in f.ch) if (f.ch[k].group === $0 && !f.ch[k].done) apply(f.ch[k]); }
  }, hid(ARG(0)), ARG(1) & 0xFF);
  OK;
}
void imp__ZN4FMOD14ChannelControl9setVolumeEf(void) {
  float v; uint32_t bits = ARG(1); memcpy(&v, &bits, 4);
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; if (!f) return; var o = f.ch[$0] || f.grp[$0];
    if (o) o.gain.gain.value = Math.max(0, $1);
  }, hid(ARG(0)), (double)v);
  OK;
}
void imp__ZN4FMOD7Channel11setPositionEjj(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; var c = f && f.ch[$0]; if (!c) return;
    var wasPlaying = !!c.src; c.halt(); c.offset = $2 === 1 ? $1 / 1000 : $1 / 44100; if (wasPlaying) c.start();
  }, hid(ARG(0)), ARG(1), ARG(2));
  OK;
}
void imp__ZN4FMOD7Channel11setPriorityEi(void) { OK; }
void imp__ZN4FMOD7Channel12getFrequencyEPf(void) {
  double r = MAIN_THREAD_EM_ASM_DOUBLE({ var f = Module.__fm; var c = f && f.ch[$0]; return c ? c.rate : 1; }, hid(ARG(0)));
  WF32(ARG(1), (float)(44100.0 * r)); OK;
}
void imp__ZN4FMOD7Channel12setFrequencyEf(void) {
  float v; uint32_t bits = ARG(1); memcpy(&v, &bits, 4);
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; var c = f && f.ch[$0]; if (!c) return;
    c.rate = $1 > 0 ? $1 / 44100 : 1; if (c.src) c.src.playbackRate.value = c.rate;
  }, hid(ARG(0)), (double)v);
  OK;
}
void imp__ZN4FMOD7Channel12setLoopCountEi(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; var c = f && f.ch[$0]; if (!c) return;
    c.loop = $1 !== 0; if (c.src) c.src.loop = c.loop;
  }, hid(ARG(0)), (int)ARG(1));
  OK;
}
void imp__ZN4FMOD7Channel15setChannelGroupEPNS_12ChannelGroupE(void) {
  MAIN_THREAD_ASYNC_EM_ASM({
    var f = Module.__fm; var c = f && f.ch[$0]; if (!c) return; var g = f.grp[$1];
    c.gain.disconnect(); c.gain.connect(g ? g.gain : f.master); c.group = $1;
  }, hid(ARG(0)), hid(ARG(1)));
  OK;
}
void imp__ZN4FMOD12ChannelGroup7releaseEv(void) { OK; }
