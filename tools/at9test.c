/* Host test for the ATRAC9 encoder: at9test stock.at9 in.raw out.at9  (in.raw = 48 kHz 16-bit stereo, little endian) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "../src/atrac9.h"

static uint8_t *slurp(const char *p, size_t *n) {
  FILE *f = fopen(p, "rb");
  if (!f) return NULL;
  fseek(f, 0, SEEK_END);
  *n = (size_t)ftell(f);
  fseek(f, 0, SEEK_SET);
  uint8_t *b = malloc(*n);
  if (fread(b, 1, *n, f) != *n) return NULL;
  fclose(f);
  return b;
}

static int prog(void *c, double f) {
  (void)c;
  fprintf(stderr, "\r%3.0f%%", f * 100);
  return 0;
}

int main(int argc, char **argv) {
  if (argc < 4) return 2;
  size_t sn, rn;
  uint8_t *stock = slurp(argv[1], &sn), *raw = slurp(argv[2], &rn);
  if (!stock || !raw) return 3;
  long n = (long)(rn / 4);
  int16_t *l = malloc(sizeof(int16_t) * (size_t)n), *r = malloc(sizeof(int16_t) * (size_t)n);
  for (long i = 0; i < n; i++) {
    memcpy(&l[i], raw + i * 4, 2);
    memcpy(&r[i], raw + i * 4 + 2, 2);
  }
  uint8_t *out;
  size_t on;
  char err[300] = "";
  clock_t t0 = clock();
  int rc = a9_encode(stock, sn, l, r, (size_t)n, 0, prog, NULL, &out, &on, err, sizeof(err));
  fprintf(stderr, "\n%s %.1fs\n", rc ? err : "ok", (double)(clock() - t0) / CLOCKS_PER_SEC);
  if (rc) return 1;
  FILE *f = fopen(argv[3], "wb");
  fwrite(out, 1, on, f);
  fclose(f);
  return 0;
}
