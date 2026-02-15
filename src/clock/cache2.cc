#include "clock/cache2.h"

#include <memory>

#include "clock/shard.h"

namespace hyper_cache::clock {

Cache2::Cache2(const CacheOptions2& options) : options_(options) {
    shards_.reserve(options.shard_num);

    ClockCacheOptions shard_options = options;
    shard_options.capacity          = options.capacity / options.shard_num;
    shard_options.length            = options.length / options.shard_num;

    for (int i = 0; i < options.shard_num; ++i) {
        shards_.emplace_back(std::make_unique<ShardWrapper>(shard_options));
    }
}

Cache2::~Cache2() { shards_.clear(); }

HandleImpl* Cache2::Lookup(void* key, int32_t key_size) {
    const UniqueId64x2 hashed_key = HashKey(key, key_size);
    return GetShard(hashed_key)->Lookup(hashed_key);
}

bool Cache2::Insert(void* key, int32_t key_size, const Handle& handle) {
    const UniqueId64x2 hashed_key            = HashKey(key, key_size);
    const_cast<Handle*>(&handle)->hashed_key = hashed_key;
    return GetShard(hashed_key)->Insert(handle);
}

bool Cache2::Release(HandleImpl* handle) {
    const UniqueId64x2 hashed_key = handle->GetHash();
    return GetShard(hashed_key)->Release(handle);
}

bool Cache2::Erase(const UniqueId64x2& hashed_key) { return GetShard(hashed_key)->Erase(hashed_key); }

size_t Cache2::GetOccupancy() const {
    size_t occupancy = 0;
    for (auto& shard : shards_) {
        occupancy += shard->GetOccupancy();
    }
    return occupancy;
}

size_t Cache2::GetUsage() const {
    size_t usage = 0;
    for (auto& shard : shards_) {
        usage += shard->GetUsage();
    }
    return usage;
}

size_t Cache2::GetLength() const {
    size_t table_size = 0;
    for (auto& shard : shards_) {
        table_size += shard->GetLength();
    }
    return table_size;
}

size_t Cache2::GetOccupancyLimit() const {
    size_t occupancy_limit = 0;
    for (auto& shard : shards_) {
        occupancy_limit += shard->GetOccupancyLimit();
    }
    return occupancy_limit;
}

const std::vector<std::unique_ptr<ShardWrapper>>& Cache2::GetShards() const { return shards_; }

ShardWrapper* Cache2::GetShard(size_t shard_idx) { return shards_[shard_idx].get(); }

ShardWrapper* Cache2::GetShard(const UniqueId64x2& hashed_key) {
    const int32_t shard_idx = hashed_key[0] % shards_.size();
    return GetShard(shard_idx);
}

UniqueId64x2 Cache2::HashKey(const void* key, int32_t key_size) {
    return options_.hash_key_fn(key, key_size, options_.hash_seed);
}

}  // namespace hyper_cache::clock