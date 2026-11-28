// Known-answer tests that call lifted library functions directly (lifter debugging aid).
// Run: SELFTEST=1 ANDWASM_ROOT=... node <build>/node/index.js
#include "rt.h"
#pragma clang diagnostic ignored "-Wint-conversion"
#include <stdio.h>
#include <stdlib.h>

const char *rt_fs_root(void);
static uint32_t gbuf(uint32_t n) { return (uint32_t)(uintptr_t)calloc(1, n); }

static void hexdump(const char *label, uint32_t a, int n) {
  fprintf(stderr, "%s", label); for (int i = 0; i < n; i++) fprintf(stderr, "%02x", M8(a + i)); fprintf(stderr, "\n");
}

void rt_selftest(void) {
  uint32_t f;
  if ((f = rt_export("crc32"))) {
    uint32_t s = gbuf(16); memcpy(P(s), "123456789", 9);
    uint32_t r = rt_call(f, 3, 0u, s, 9u);
    fprintf(stderr, "[selftest] crc32(\"123456789\") = %08x (expect cbf43926) %s\n", r, r == 0xcbf43926u ? "OK" : "FAIL");
  } else fprintf(stderr, "[selftest] crc32 not exported\n");

  uint32_t fk = rt_export("aes_encrypt_key128"), fe = rt_export("aes_encrypt");
  if (fk && fe) {
    uint32_t key = gbuf(16), in = gbuf(16), out = gbuf(16), ctx = gbuf(1024);
    for (int i = 0; i < 16; i++) { W8(key + i, i); W8(in + i, i * 0x11); }
    uint32_t r1 = rt_call(fk, 2, key, ctx);
    uint32_t r2 = rt_call(fe, 3, in, out, ctx);
    hexdump("[selftest] aes128 got    ", out, 16);
    fprintf(stderr, "[selftest] aes128 expect 69c4e0d86a7b0430d8cdb78070b4c55a (key rc=%d enc rc=%d)\n", (int)r1, (int)r2);
  } else fprintf(stderr, "[selftest] aes not exported (%x %x)\n", fk, fe);

  if ((f = rt_export("uncompress"))) {
    char p[512]; snprintf(p, sizeof p, "%s/selftest.z", rt_fs_root());
    FILE *fp = fopen(p, "rb");
    if (fp) {
      fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
      uint32_t src = gbuf(n); fread(P(src), 1, n, fp); fclose(fp);
      uint32_t dst = gbuf(1 << 20), dlen = gbuf(4); W32(dlen, 1 << 20);
      uint32_t r = rt_call(f, 4, dst, dlen, src, (uint32_t)n);
      uint32_t ok = 1; for (uint32_t i = 0; i < M32(dlen); i++) if (M8(dst + i) != (uint8_t)((i * 7 + i / 13) & 0xFF)) { ok = 0; fprintf(stderr, "[selftest] uncompress mismatch at %u\n", i); break; }
      fprintf(stderr, "[selftest] uncompress rc=%d len=%u %s\n", (int)r, M32(dlen), r == 0 && ok && M32(dlen) == 200000 ? "OK" : "FAIL");
    } else fprintf(stderr, "[selftest] no %s\n", p);
  } else fprintf(stderr, "[selftest] uncompress not exported\n");
}

// Whole WinZip-AES read of the game's archive through the lifted minizip (SELFTEST_ZIP=<guest path>).
void rt_selftest_zip(void) {
  const char *zp = getenv("SELFTEST_ZIP"); if (!zp) return;
  char pw[512] = ""; char p[512]; snprintf(p, sizeof p, "%s/andwasm.cfg", rt_fs_root());
  FILE *cf = fopen(p, "r"); char line[1024];
  while (cf && fgets(line, sizeof line, cf)) if (!strncmp(line, "PTJniHelper.password=", 21)) { strncpy(pw, line + 21, sizeof pw - 1); pw[strcspn(pw, "\r\n")] = 0; }
  if (cf) fclose(cf);
  uint32_t fo = rt_export("unzOpen"), ff = rt_export("unzGoToFirstFile"), fop = rt_export("unzOpenCurrentFilePassword"),
           fr = rt_export("unzReadCurrentFile"), fc = rt_export("unzCloseCurrentFile");
  uint32_t path = gbuf(512); strcpy((char *)P(path), zp);
  uint32_t gpw = gbuf(512); strcpy((char *)P(gpw), pw);
  uint32_t z = rt_call(fo, 1, path);
  fprintf(stderr, "[selftest] unzOpen(%s) -> %x\n", zp, z);
  if (!z) return;
  fprintf(stderr, "[selftest] first file rc=%d\n", (int)rt_call(ff, 1, z));
  int r = (int)rt_call(fop, 2, z, gpw);
  fprintf(stderr, "[selftest] openCurrentFilePassword rc=%d (pw len %zu)\n", r, strlen(pw));
  uint32_t buf = gbuf(1 << 20); int total = 0, n;
  while ((n = (int)rt_call(fr, 3, z, buf, 65536u)) > 0) total += n;
  fprintf(stderr, "[selftest] read total=%d last rc=%d\n", total, n);
  hexdump("[selftest] first bytes ", buf, 24);
  fprintf(stderr, "[selftest] closeCurrentFile rc=%d (-105 = CRC error)\n", (int)rt_call(fc, 1, z));
}

// Differential testing (recomp/arm/difftest): SELFTEST_CALLS=<calls file> SELFTEST_OUT=<results file>
// runs each call from gen_tests.py through the lifted code and writes `r0 r1 <buffers hex>` per line.
static int hexval(int c) { return c <= '9' ? c - '0' : (c | 32) - 'a' + 10; }
void rt_selftest_calls(void) {
  const char *in = getenv("SELFTEST_CALLS"), *outp = getenv("SELFTEST_OUT");
  if (!in || !outp) return;
  FILE *f = fopen(in, "r"), *o = fopen(outp, "w");
  if (!f || !o) { fprintf(stderr, "[difftest] cannot open %s / %s\n", in, outp); return; }
  static char line[1 << 16];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    char *p1 = strchr(line, '|'); if (!p1) continue;
    char *p2 = strchr(p1 + 1, '|'); if (!p2) continue;
    *p1 = 0; *p2 = 0;
    uint32_t addr = (uint32_t)strtoul(line, NULL, 16);
    uint32_t bufs[8] = {0}; int nb = 0, lens[8] = {0};
    char *b = p2 + 1;
    while (*b && nb < 8) {
      while (*b == ' ' || *b == '\n') b++;
      if (!*b || *b == '-') break;
      char *e = b; while (*e && *e != ' ' && *e != '\n') e++;
      int len = (int)(e - b) / 2;
      bufs[nb] = (uint32_t)(uintptr_t)calloc(1, 4096);
      for (int k = 0; k < len; k++) W8(bufs[nb] + k, hexval(b[2 * k]) * 16 + hexval(b[2 * k + 1]));
      lens[nb++] = len; b = e;
    }
    uint32_t w[8] = {0}; int nw = 0;
    char *a = p1 + 1, *tok;
    while ((tok = strtok(a, " \n")) && nw < 8) { a = NULL; w[nw++] = tok[0] == 'B' ? bufs[atoi(tok + 1)] : (uint32_t)strtoul(tok, NULL, 16); }
    uint64_t r = rt_call64(B + addr, nw, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    fprintf(o, "%08x %08x", (uint32_t)r, (uint32_t)(r >> 32));
    for (int k = 0; k < nb; k++) { fputc(' ', o); for (int j = 0; j < lens[k]; j++) fprintf(o, "%02x", M8(bufs[k] + j)); free((void *)P(bufs[k])); }
    fputc('\n', o); n++;
  }
  fclose(f); fclose(o);
  fprintf(stderr, "[difftest] %d calls -> %s\n", n, outp);
}
