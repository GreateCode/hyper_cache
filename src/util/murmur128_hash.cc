// murmur128_hash.h
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "util/murmur128_hash.h"

namespace hyper_cache {

static inline uint64_t rotl64(uint64_t x, int8_t r) {
  return (x << r) | (x >> (64 - r));
}

static inline uint64_t fmix64(uint64_t k) {
  k ^= k >> 33;
  k *= 0xff51afd7ed558ccdULL;
  k ^= k >> 33;
  k *= 0xc4ceb9fe1a85ec53ULL;
  k ^= k >> 33;
  return k;
}

// MurmurHash3_x64_128 (public domain, by Austin Appleby).
// Returns 128-bit hash in (out_h1, out_h2).
void MurmurHash3_x64_128(const void *key, int32_t len, uint32_t seed,
                         uint64_t *out_h1, uint64_t *out_h2) {
  const uint8_t *data = static_cast<const uint8_t *>(key);
  const int nblocks = len / 16;

  uint64_t h1 = seed;
  uint64_t h2 = seed;

  const uint64_t c1 = 0x87c37b91114253d5ULL;
  const uint64_t c2 = 0x4cf5ad432745937fULL;

  // body
  const uint64_t *blocks = reinterpret_cast<const uint64_t *>(data);
  for (int i = 0; i < nblocks; ++i) {
    uint64_t k1 = blocks[2 * i + 0];
    uint64_t k2 = blocks[2 * i + 1];

    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
    h1 = rotl64(h1, 27);
    h1 += h2;
    h1 = h1 * 5 + 0x52dce729ULL;

    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
    h2 = rotl64(h2, 31);
    h2 += h1;
    h2 = h2 * 5 + 0x38495ab5ULL;
  }

  // tail
  const uint8_t *tail = data + nblocks * 16;
  uint64_t k1 = 0;
  uint64_t k2 = 0;

  switch (len & 15) {
  case 15:
    k2 ^= static_cast<uint64_t>(tail[14]) << 48;
    [[fallthrough]];
  case 14:
    k2 ^= static_cast<uint64_t>(tail[13]) << 40;
    [[fallthrough]];
  case 13:
    k2 ^= static_cast<uint64_t>(tail[12]) << 32;
    [[fallthrough]];
  case 12:
    k2 ^= static_cast<uint64_t>(tail[11]) << 24;
    [[fallthrough]];
  case 11:
    k2 ^= static_cast<uint64_t>(tail[10]) << 16;
    [[fallthrough]];
  case 10:
    k2 ^= static_cast<uint64_t>(tail[9]) << 8;
    [[fallthrough]];
  case 9:
    k2 ^= static_cast<uint64_t>(tail[8]) << 0;
    k2 *= c2;
    k2 = rotl64(k2, 33);
    k2 *= c1;
    h2 ^= k2;
    [[fallthrough]];
  case 8:
    k1 ^= static_cast<uint64_t>(tail[7]) << 56;
    [[fallthrough]];
  case 7:
    k1 ^= static_cast<uint64_t>(tail[6]) << 48;
    [[fallthrough]];
  case 6:
    k1 ^= static_cast<uint64_t>(tail[5]) << 40;
    [[fallthrough]];
  case 5:
    k1 ^= static_cast<uint64_t>(tail[4]) << 32;
    [[fallthrough]];
  case 4:
    k1 ^= static_cast<uint64_t>(tail[3]) << 24;
    [[fallthrough]];
  case 3:
    k1 ^= static_cast<uint64_t>(tail[2]) << 16;
    [[fallthrough]];
  case 2:
    k1 ^= static_cast<uint64_t>(tail[1]) << 8;
    [[fallthrough]];
  case 1:
    k1 ^= static_cast<uint64_t>(tail[0]) << 0;
    k1 *= c1;
    k1 = rotl64(k1, 31);
    k1 *= c2;
    h1 ^= k1;
    break;
  case 0:
    break;
  }

  // finalization
  h1 ^= static_cast<uint64_t>(len);
  h2 ^= static_cast<uint64_t>(len);

  h1 += h2;
  h2 += h1;

  h1 = fmix64(h1);
  h2 = fmix64(h2);

  h1 += h2;
  h2 += h1;

  *out_h1 = h1;
  *out_h2 = h2;
}

} // namespace hyper_cache