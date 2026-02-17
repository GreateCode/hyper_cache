#pragma once

#include <vector>

#include "clock/handle.h"
#include "clock/shard.h"

namespace hyper_cache::clock {

struct CacheOptions2 : public ClockCacheOptions {
    size_t shard_num  = 1;  // number of shards, suggest 2^n
    int32_t hash_seed = -2;
};

class Cache2 {
public:
    Cache2(const CacheOptions2& options);
    ~Cache2();

    bool Lookup(void* key, int32_t key_size, HandlePin* handle_pin);

    bool Insert(void* key, int32_t key_size, const Handle& handle);

    bool Release(HandlePin* handle_pin);

    bool Erase(const UniqueId64x2& hashed_key);

    size_t GetOccupancy() const;

    size_t GetUsage() const;

    size_t GetLength() const;

    size_t GetOccupancyLimit() const;

    ShardWrapper* GetShard(size_t shard_idx);

    const std::vector<std::unique_ptr<ShardWrapper>>& GetShards() const;

private:
    UniqueId64x2 HashKey(const void* key, int32_t key_size);
    ShardWrapper* GetShard(const UniqueId64x2& hashed_key);

private:
    std::vector<std::unique_ptr<ShardWrapper>> shards_;
    CacheOptions2 options_;
};

}  // namespace hyper_cache::clock
