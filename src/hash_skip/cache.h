#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <thread>
#include <vector>

#include "hash_skip/atomic_shared_ptr.h"
#include "hash_skip/atomic_skiplist.h"
#include "util/atomic.h"
#include "util/queue.h"
#include "util/util.h"

namespace hyper_cache::hash_skip {

struct HashSkipCacheOptions {
  size_t capacity = 0;   // total capacity of the cache
  size_t length = 0;     // number of slots in the cache
  size_t value_size = 0; // only set one of length , value_size.

  // use for accelerate the hit count decrease speed, when the hit count is too
  // large, the decrease speed will be too slow
  int32_t hit_decr_delta_min = 1;
};

struct LowKey {
  int32_t bucket_index;
  int64_t gen; // the generation of the bucket when pushed
  int64_t key;
};

class HashSkipCache {
public:
  HashSkipCache(int64_t capicity, int32_t bucket_num)
      : capicity_(capicity), buckets_(bucket_num),
        buckets_gen_(buckets_.size()), low_queue_(10000) {
    for (int32_t i = 0; i < buckets_.size(); i++) {
      buckets_gen_[i].StoreRelaxed(0);
    }
    decay_thread_ = std::thread([this] { Decay(); });
    compact_thread_ = std::thread([this] { Compact(); });
  }
  ~HashSkipCache() {
    // ensure the resources are released in order
    stop_.StoreRelaxed(true);
    decay_thread_.join();
    compact_thread_.join();
  }

  bool Init() {
    if (init_) {
      return true;
    }
    for (int32_t i = 0; i < buckets_.size(); i++) {
      buckets_[i].Store(std::make_shared<AtomicSkipList>());
    }
    init_ = true;
    return true;
  }

  int64_t GetCapicity() const { return capicity_; }
  int64_t GetMemoryUsed() const { return memory_used_.LoadRelaxed(); }
  int64_t GetLowQueueSize() const { return low_queue_.Size(); }

  std::shared_ptr<Handle> Get(int64_t key) {
    int64_t bucket_index = key % buckets_.size();
    std::shared_ptr<Handle> handle;
    std::shared_ptr<AtomicSkipList> bucket = buckets_[bucket_index].Load();
    if (bucket && bucket->Get(key, &handle)) {
      return handle;
    }
    return nullptr;
  }

  bool Insert(int64_t key, std::shared_ptr<Handle> handle) {
    int32_t charge = handle ? handle->Charge() : 0;
    int64_t bucket_index = key % buckets_.size();
    std::shared_ptr<AtomicSkipList> bucket = buckets_[bucket_index].Load();
    if (!bucket) {
      return false;
    }
    if (memory_used_.LoadRelaxed() + charge > capicity_) {
      if (!Evict(charge)) {
        return false;
      }
    }
    std::shared_ptr<Handle> old_handle = bucket->Insert(key, handle);
    if (old_handle) {
      charge -= old_handle->Charge();
    }
    memory_used_.FetchAddRelaxed(charge);
    return true;
  }

  void PrintBucketsStatus() {
    for (int32_t i = 0; i < buckets_.size(); i++) {
      std::shared_ptr<AtomicSkipList> bucket = buckets_[i].Load();
      std::cout << "bucket:" << i << ", size:" << bucket->GetNodeNum()
                << ", marked_deleted_num:" << bucket->GetMarkedDeletedNum()
                << std::endl;
    }
  }

private:
  void Decay() {
    int32_t bucket_index = 0;
    while (true) {
      if (bucket_index == buckets_.size()) {
        bucket_index = 0;
      }

      while (!stop_.LoadRelaxed() && !ShouldDecay()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (stop_.LoadRelaxed()) {
        return;
      }

      DecayOneBucket(bucket_index);
      bucket_index++;
    }
  }

  void DecayOneBucket(int32_t bucket_index) {
    std::shared_ptr<AtomicSkipList> bucket = buckets_[bucket_index].Load();
    if (!bucket) {
      return;
    }

    int16_t delta = 100;
    auto fn = [&](Node *node) -> bool {
      const int16_t hit_count = node->DecrHitCount(delta);
      if (hit_count > 100) {
        return true;
      }
      if (hit_count > 0) {
        const int64_t gen = buckets_gen_[bucket_index].LoadAcquire();
        low_queue_.Push(LowKey{bucket_index, gen, node->key});
        return true;
      }
      const int32_t free_charge = bucket->MarkDeleted(node);
      if (free_charge > 0) {
        memory_used_.FetchSubRelaxed(free_charge);
      }
      return true;
    };
    bucket->IterateLevel(fn, 0);
  }

  bool ShouldDecay() {
    bool enbale_decay = memory_used_.LoadRelaxed() > capicity_ * 0.7;
    if (!enbale_decay) {
      enbale_decay = low_queue_.Size() * 1.0 / low_queue_.Capicity() < 0.7;
    }
    return enbale_decay;
  }

  void Compact() {
    int32_t bucket_index = 0;
    while (!stop_.LoadRelaxed()) {
      if (bucket_index == buckets_.size()) {
        bucket_index = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));

      std::shared_ptr<AtomicSkipList> new_bucket =
          CompactOneBucket(bucket_index);
      if (new_bucket) {
        buckets_[bucket_index].Store(new_bucket);
        buckets_gen_[bucket_index].FetchAddRelaxed(1);
      }
      bucket_index++;
    }
  }

  std::shared_ptr<AtomicSkipList> CompactOneBucket(int32_t bucket_index) {
    std::shared_ptr<AtomicSkipList> bucket = buckets_[bucket_index].Load();
    if (bucket == nullptr ||
        (bucket->GetMarkedDeletedNum() * 1.0 / bucket->GetNodeNum() < 0.3)) {
      return nullptr;
    }
    std::shared_ptr<AtomicSkipList> new_bucket =
        std::make_shared<AtomicSkipList>();
    auto fn = [&new_bucket](Node *node) -> bool {
      new_bucket->Insert(node->key, node->GetHandle());
      return true;
    };
    bucket->IterateLevel(fn, 0);
    return new_bucket;
  }

  bool Evict(int32_t need_charge) {
    LowKey it;
    while (need_charge > 0 && low_queue_.Size() > 0) {
      if (!low_queue_.Pop(&it)) {
        continue;
      }
      if (it.gen != buckets_gen_[it.bucket_index].LoadAcquire()) {
        continue;
      }
      std::shared_ptr<AtomicSkipList> bucket = buckets_[it.bucket_index].Load();
      if (!bucket) {
        continue;
      }
      Node *node = bucket->Get(it.key);
      if (unlikely(!node)) {
        continue;
      }
      const int32_t free_charge = node->MarkDeleted();
      if (free_charge > 0) {
        memory_used_.FetchAddRelaxed(free_charge);
        need_charge -= free_charge;
      }
    }
    return true;
  }

private:
  int64_t capicity_{0};
  AcqRelAtomic<int64_t> memory_used_{0};
  std::vector<AtomicSharedPtr<AtomicSkipList>> buckets_;
  std::vector<AcqRelAtomic<int64_t>> buckets_gen_; // same length as buckets_
  Queue<LowKey> low_queue_;
  bool init_ = false;

  AcqRelAtomic<bool> stop_{false};
  std::thread compact_thread_;
  std::thread decay_thread_;
};

} // namespace hyper_cache::hash_skip
