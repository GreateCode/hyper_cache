#include "clock/cache.h"

#include <cstdint>
#include <iostream>
#include <memory>

#include "clock/handle.h"

namespace hyper_cache::clock {

Cache::Cache(const CacheOptions& options) : options_(options) {
    shards_.reserve(options.shard_num);

    ClockCacheOptions shard_options = options;
    shard_options.capacity          = options.capacity / options.shard_num;
    shard_options.length            = options.length / options.shard_num;

    for (int i = 0; i < options.shard_num; ++i) {
        shards_.emplace_back(std::make_unique<ShardWrapper>(shard_options));
    }

    stop_scaling_.StoreRelease(false);
    scale_thread_ = std::thread(&Cache::SmoothScale, this);
}

Cache::~Cache() {
    stop_scaling_.StoreRelease(true);
    scale_thread_.join();
    shards_.clear();
}

bool Cache::Lookup(void* key, int32_t key_size, HandlePin* handle_pin) {
    const UniqueId64x2 hashed_key = HashKey(key, key_size);
    return GetShard(hashed_key)->Lookup(hashed_key, handle_pin);
}

HandleImpl* Cache::Lookup(void* key, int32_t key_size) {
    const UniqueId64x2 hashed_key = HashKey(key, key_size);
    return GetShard(hashed_key)->Lookup(hashed_key);
}

bool Cache::Insert(void* key, int32_t key_size, const Handle& handle) {
    const UniqueId64x2 hashed_key            = HashKey(key, key_size);
    const_cast<Handle*>(&handle)->hashed_key = hashed_key;
    return GetShard(hashed_key)->Insert(handle);
}

bool Cache::Release(HandlePin* handle_pin) {
    if (!handle_pin || !handle_pin->handle) {
        return false;
    }
    return GetShard(handle_pin->handle->GetHash())->Release(handle_pin);
}

bool Cache::Release(HandleImpl* handle) {
    if (!handle) {
        return false;
    }
    return GetShard(handle->GetHash())->Release(handle);
}

size_t Cache::GetUsage() const {
    size_t usage = 0;
    for (auto& shard : shards_) {
        usage += shard->GetUsage();
    }
    return usage;
}

size_t Cache::GetLength() const {
    size_t table_size = 0;
    for (auto& shard : shards_) {
        table_size += shard->GetLength();
    }
    return table_size;
}

size_t Cache::GetOccupancyLimit() const {
    size_t occupancy_limit = 0;
    for (auto& shard : shards_) {
        occupancy_limit += shard->GetOccupancyLimit();
    }
    return occupancy_limit;
}

const std::vector<std::unique_ptr<ShardWrapper>>& Cache::GetShards() const { return shards_; }

ShardWrapper* Cache::GetShard(size_t shard_idx) { return shards_[shard_idx].get(); }

ShardWrapper* Cache::GetShard(const UniqueId64x2& hashed_key) {
    const int32_t shard_idx = hashed_key[0] % shards_.size();
    return GetShard(shard_idx);
}

UniqueId64x2 Cache::HashKey(const void* key, int32_t key_size) {
    return options_.hash_key_fn(key, key_size, options_.hash_seed);
}

void Cache::SmoothScale() {
    while (!stop_scaling_.LoadAcquire()) {
        int32_t idx = 0;
        for (auto& shard : shards_) {
            if (stop_scaling_.LoadAcquire()) {
                return;
            }
            const int32_t old_length                               = shard->GetLength();
            const std::chrono::steady_clock::time_point start_time = std::chrono::steady_clock::now();
            const bool succ                                        = shard->SmoothScale();
            const std::chrono::duration<double> duration           = std::chrono::steady_clock::now() - start_time;
            if (succ) {
                std::cout << "shard " << idx << " smooth scale time: " << duration.count()
                          << " seconds, old length: " << old_length << ", new length: " << shard->GetLength()
                          << std::endl;
            }
            ++idx;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

}  // namespace hyper_cache::clock