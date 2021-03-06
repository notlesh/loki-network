
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <sodium/core.h>
#include <sodium/crypto_stream_chacha20.h>
#include <sodium/private/common.h>
#include <sodium/private/sse2_64_32.h>
#include <sodium/utils.h>

#if __AVX2__
#ifdef __GNUC__
#pragma GCC target("sse2")
#pragma GCC target("ssse3")
#pragma GCC target("sse4.1")
#pragma GCC target("avx2")
#endif

#include <emmintrin.h>
#include <immintrin.h>
#include <smmintrin.h>
#include <tmmintrin.h>

#ifndef __amd64__
#ifdef __clang__
#define __DEFAULT_FN_ATTRS \
  __attribute__((__always_inline__, __nodebug__, __target__("sse2")))
#else
#define __DEFAULT_FN_ATTRS \
  __attribute__((__always_inline__, __target__("sse2")))
#endif

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_cvtsi64_si128(long long __a)
{
  return (__m128i){__a, 0};
}
#endif

#include "../stream_chacha20.h"
#include "chacha20_dolbeau-avx2.h"

#define ROUNDS 20

typedef struct chacha_ctx
{
  uint32_t input[16];
} chacha_ctx;

static void
chacha_keysetup(chacha_ctx *ctx, const uint8_t *k)
{
  ctx->input[0]  = 0x61707865;
  ctx->input[1]  = 0x3320646e;
  ctx->input[2]  = 0x79622d32;
  ctx->input[3]  = 0x6b206574;
  ctx->input[4]  = LOAD32_LE(k + 0);
  ctx->input[5]  = LOAD32_LE(k + 4);
  ctx->input[6]  = LOAD32_LE(k + 8);
  ctx->input[7]  = LOAD32_LE(k + 12);
  ctx->input[8]  = LOAD32_LE(k + 16);
  ctx->input[9]  = LOAD32_LE(k + 20);
  ctx->input[10] = LOAD32_LE(k + 24);
  ctx->input[11] = LOAD32_LE(k + 28);
}

static void
chacha_ivsetup(chacha_ctx *ctx, const uint8_t *iv, const uint8_t *counter)
{
  ctx->input[12] = counter == NULL ? 0 : LOAD32_LE(counter + 0);
  ctx->input[13] = counter == NULL ? 0 : LOAD32_LE(counter + 4);
  ctx->input[14] = LOAD32_LE(iv + 0);
  ctx->input[15] = LOAD32_LE(iv + 4);
}

static void
chacha_ietf_ivsetup(chacha_ctx *ctx, const uint8_t *iv, const uint8_t *counter)
{
  ctx->input[12] = counter == NULL ? 0 : LOAD32_LE(counter);
  ctx->input[13] = LOAD32_LE(iv + 0);
  ctx->input[14] = LOAD32_LE(iv + 4);
  ctx->input[15] = LOAD32_LE(iv + 8);
}

static void
chacha20_encrypt_bytes(chacha_ctx *ctx, const uint8_t *m, uint8_t *c,
                       unsigned long long bytes)
{
  uint32_t *const x = &ctx->input[0];

  if(!bytes)
  {
    return; /* LCOV_EXCL_LINE */
  }
  if(bytes > crypto_stream_chacha20_MESSAGEBYTES_MAX)
  {
    sodium_misuse();
  }
#include "u8.h"
#include "u4.h"
#include "u1.h"
#include "u0.h"
}

static int
stream_ref(unsigned char *c, unsigned long long clen, const unsigned char *n,
           const unsigned char *k)
{
  struct chacha_ctx ctx;

  if(!clen)
  {
    return 0;
  }
  COMPILER_ASSERT(crypto_stream_chacha20_KEYBYTES == 256 / 8);
  chacha_keysetup(&ctx, k);
  chacha_ivsetup(&ctx, n, NULL);
  memset(c, 0, clen);
  chacha20_encrypt_bytes(&ctx, c, c, clen);
  sodium_memzero(&ctx, sizeof ctx);

  return 0;
}

static int
stream_ietf_ref(unsigned char *c, unsigned long long clen,
                const unsigned char *n, const unsigned char *k)
{
  struct chacha_ctx ctx;

  if(!clen)
  {
    return 0;
  }
  COMPILER_ASSERT(crypto_stream_chacha20_KEYBYTES == 256 / 8);
  chacha_keysetup(&ctx, k);
  chacha_ietf_ivsetup(&ctx, n, NULL);
  memset(c, 0, clen);
  chacha20_encrypt_bytes(&ctx, c, c, clen);
  sodium_memzero(&ctx, sizeof ctx);

  return 0;
}

static int
stream_ref_xor_ic(unsigned char *c, const unsigned char *m,
                  unsigned long long mlen, const unsigned char *n, uint64_t ic,
                  const unsigned char *k)
{
  struct chacha_ctx ctx;
  uint8_t ic_bytes[8];
  uint32_t ic_high;
  uint32_t ic_low;

  if(!mlen)
  {
    return 0;
  }
  ic_high = (uint32_t)(ic >> 32);
  ic_low  = (uint32_t)ic;
  STORE32_LE(&ic_bytes[0], ic_low);
  STORE32_LE(&ic_bytes[4], ic_high);
  chacha_keysetup(&ctx, k);
  chacha_ivsetup(&ctx, n, ic_bytes);
  chacha20_encrypt_bytes(&ctx, m, c, mlen);
  sodium_memzero(&ctx, sizeof ctx);

  return 0;
}

static int
stream_ietf_ref_xor_ic(unsigned char *c, const unsigned char *m,
                       unsigned long long mlen, const unsigned char *n,
                       uint32_t ic, const unsigned char *k)
{
  struct chacha_ctx ctx;
  uint8_t ic_bytes[4];

  if(!mlen)
  {
    return 0;
  }
  STORE32_LE(ic_bytes, ic);
  chacha_keysetup(&ctx, k);
  chacha_ietf_ivsetup(&ctx, n, ic_bytes);
  chacha20_encrypt_bytes(&ctx, m, c, mlen);
  sodium_memzero(&ctx, sizeof ctx);

  return 0;
}

struct crypto_stream_chacha20_implementation
    crypto_stream_chacha20_dolbeau_avx2_implementation = {
        SODIUM_C99(.stream =) stream_ref,
        SODIUM_C99(.stream_ietf =) stream_ietf_ref,
        SODIUM_C99(.stream_xor_ic =) stream_ref_xor_ic,
        SODIUM_C99(.stream_ietf_xor_ic =) stream_ietf_ref_xor_ic};

#endif
