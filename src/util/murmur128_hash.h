// murmur128_hash.h
#pragma once

#include <cstdint>

namespace cache {

// MurmurHash3_x64_128 (public domain, by Austin Appleby).
// Returns 128-bit hash in (out_h1, out_h2).
void MurmurHash3_x64_128(const void *key, int32_t len, uint32_t seed,
                         uint64_t *out_h1, uint64_t *out_h2);

} // namespace cache