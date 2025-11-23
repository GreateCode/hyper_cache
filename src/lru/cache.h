#pragma once

#include <iostream>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "lru/cache_shard.h"

namespace cache::lru {

struct LRUCacheOptions {
  size_t capacity = 0;   // total capacity of the cache
  int64_t entry_num = 0; // number of entries in the cache
  size_t value_size = 0; // only set one of length , value_size.
  int32_t shard_num = 1; // number of shards
};

template <typename Key> class LRUCache {
public:
  LRUCache(const LRUCacheOptions &options) {
    int64_t per_shard_capacity = options.capacity / options.shard_num;
    shards_.reserve(options.shard_num);
    for (int i = 0; i < options.shard_num; ++i) {
      CacheSharedOptions shard_options = {options.capacity, options.entry_num,
                                          options.value_size};
      auto shard = std::make_unique<LRUCacheShard<Key>>(shard_options);
      shards_.push_back(std::move(shard));
    }
  }

  size_t GetUsage() const {
    size_t usage = 0;
    for (auto &shard : shards_) {
      usage += shard->GetUsage();
    }
    return usage;
  }
  size_t GetCapacity() const {
    size_t capacity = 0;
    for (auto &shard : shards_) {
      capacity += shard->GetCapacity();
    }
    return capacity;
  }
  int64_t GetMaxEntryNum() const {
    int64_t max_entry_num = 0;
    for (auto &shard : shards_) {
      max_entry_num += shard->GetMaxEntryNum();
    }
    return max_entry_num;
  }
  size_t GetEntryNum() const {
    size_t entry_num = 0;
    for (auto &shard : shards_) {
      entry_num += shard->GetEntryNum();
    }
    return entry_num;
  }

  bool Get(const Key &key, Handle *handle) {
    int32_t index = key % shards_.size();
    return shards_[index]->Get(key, handle);
  }

  void Insert(const Key &key, const Handle &handle) {
    int32_t index = key % shards_.size();
    shards_[index]->Insert(key, handle);
  }

private:
  std::vector<std::unique_ptr<LRUCacheShard<Key>>> shards_;
};

} // namespace cache::lru
