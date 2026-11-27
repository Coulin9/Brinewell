/*
 * Copyright 2026 Turing Software LLC
 * SPDX-License-Identifier: MIT
 *
 * Minimal streaming SHA-256 for the NGX shims.  Header-only so the
 * standalone PE targets (nvngx.dll, nvapi64.dll) pick it up without a
 * new import DLL.
 */

#ifndef NPT_SHA256_H
#define NPT_SHA256_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct npt_sha256_ctx {
   uint32_t h[8];
   uint64_t total;
   uint8_t buf[64];
   size_t buf_len;
} npt_sha256_ctx;

static inline uint32_t
npt_sha256_rotr(uint32_t x, unsigned n)
{
   return (x >> n) | (x << (32 - n));
}

static void
npt_sha256_block(npt_sha256_ctx *c, const uint8_t *p)
{
   static const uint32_t k[64] = {
      0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b,
      0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
      0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7,
      0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
      0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152,
      0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
      0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
      0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
      0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
      0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
      0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f,
      0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
      0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
   };
   uint32_t w[64];
   for (unsigned i = 0; i < 16; i++)
      w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
             ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
   for (unsigned i = 16; i < 64; i++) {
      uint32_t s0 = npt_sha256_rotr(w[i - 15], 7) ^
                    npt_sha256_rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
      uint32_t s1 = npt_sha256_rotr(w[i - 2], 17) ^
                    npt_sha256_rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
      w[i] = w[i - 16] + s0 + w[i - 7] + s1;
   }
   uint32_t a = c->h[0], b = c->h[1], d = c->h[3], e = c->h[4],
            f = c->h[5], g = c->h[6], h = c->h[7], cc = c->h[2];
   for (unsigned i = 0; i < 64; i++) {
      uint32_t s1 = npt_sha256_rotr(e, 6) ^ npt_sha256_rotr(e, 11) ^
                    npt_sha256_rotr(e, 25);
      uint32_t ch = (e & f) ^ (~e & g);
      uint32_t t1 = h + s1 + ch + k[i] + w[i];
      uint32_t s0 = npt_sha256_rotr(a, 2) ^ npt_sha256_rotr(a, 13) ^
                    npt_sha256_rotr(a, 22);
      uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
      h = g; g = f; f = e; e = d + t1;
      d = cc; cc = b; b = a; a = t1 + s0 + maj;
   }
   c->h[0] += a; c->h[1] += b; c->h[2] += cc; c->h[3] += d;
   c->h[4] += e; c->h[5] += f; c->h[6] += g; c->h[7] += h;
}

static inline void
npt_sha256_init(npt_sha256_ctx *c)
{
   static const uint32_t h0[8] = {
      0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
   };
   memcpy(c->h, h0, sizeof(h0));
   c->total = 0;
   c->buf_len = 0;
}

static inline void
npt_sha256_update(npt_sha256_ctx *c, const void *data, size_t len)
{
   const uint8_t *p = data;
   c->total += len;
   if (c->buf_len) {
      size_t n = 64 - c->buf_len;
      if (n > len)
         n = len;
      memcpy(c->buf + c->buf_len, p, n);
      c->buf_len += n;
      p += n;
      len -= n;
      if (c->buf_len == 64) {
         npt_sha256_block(c, c->buf);
         c->buf_len = 0;
      }
   }
   while (len >= 64) {
      npt_sha256_block(c, p);
      p += 64;
      len -= 64;
   }
   if (len) {
      memcpy(c->buf, p, len);
      c->buf_len = len;
   }
}

static inline void
npt_sha256_final(npt_sha256_ctx *c, uint8_t out[32])
{
   uint64_t bits = c->total * 8;
   uint8_t pad = 0x80;
   npt_sha256_update(c, &pad, 1);
   pad = 0;
   while (c->buf_len != 56)
      npt_sha256_update(c, &pad, 1);
   uint8_t len_be[8];
   for (unsigned i = 0; i < 8; i++)
      len_be[i] = (uint8_t)(bits >> (56 - i * 8));
   npt_sha256_update(c, len_be, 8);
   for (unsigned i = 0; i < 8; i++) {
      out[i * 4]     = (uint8_t)(c->h[i] >> 24);
      out[i * 4 + 1] = (uint8_t)(c->h[i] >> 16);
      out[i * 4 + 2] = (uint8_t)(c->h[i] >> 8);
      out[i * 4 + 3] = (uint8_t)c->h[i];
   }
}

#endif /* NPT_SHA256_H */
