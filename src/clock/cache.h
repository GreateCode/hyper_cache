#pragma once

#include "clock/clock_cache.h"
#include <vector>

namespace hyper_cache::clock {

struct CacheOptions : public ClockCacheOptions {
  size_t shard_num = 1; // number of shards, suggest 2^n
  int32_t hash_seed = -2;
};

class Cache {
public:
  Cache(const CacheOptions &options);
  ~Cache();

  HandleImpl *Lookup(void *key, int32_t key_size);

  bool Insert(void *key, int32_t key_size, const Handle &handle);

  bool Release(HandleImpl *handle);

  bool Erase(const UniqueId64x2 &hashed_key);

  size_t GetOccupancy() const;

  size_t GetUsage() const;

  size_t GetLength() const;

  size_t GetOccupancyLimit() const;

  ClockCache *GetShard(size_t shard_idx);

  const std::vector<std::unique_ptr<ClockCache>> &GetShards() const;

private:
  UniqueId64x2 HashKey(const void *key, int32_t key_size);
  ClockCache *GetShard(const UniqueId64x2 &hashed_key);

private:
  std::vector<std::unique_ptr<ClockCache>> shards_;
  CacheOptions options_;
};

} // namespace hyper_cache::clock