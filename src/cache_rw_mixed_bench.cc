#include <cstdint>
#include <iostream>

#include <benchmark/benchmark.h>
#include <memory>
#include <queue>
#include <sstream>

#include "clock/cache.h"
#include "hash_skip/cache.h"
#include "hash_skip/node.h"
#include "lru/cache.h"
#include "util/atomic.h"
#include "util/util.h"

class Handle : public cache::hash_skip::Handle {
public:
  Handle(int64_t val) : val_(val) {}
  int32_t Charge() const override { return sizeof(val_); }
  int64_t Val() const { return val_; }

private:
  int64_t val_;
};

const int64_t capicity = 3250000 * 16 * 2;

cache::lru::LRUCacheOptions lru_options = {
    .capacity = capicity,
    .entry_num = 8388608,
    .shard_num = 128,
};
cache::lru::LRUCache<int64_t> lru_cache(lru_options);
cache::hash_skip::HashSkipCache hs_cache(capicity, 10000 * 600);

cache::clock::CacheOptions cc_options = {{
                                             .capacity = capicity,
                                             .length = 8388608,
                                             .value_size = 16,
                                         },
                                         .shard_num = 128};
cache::clock::Cache clock_cache(cc_options);

cache::ThreadSafeNormalRandom rnd(0.0, 10.0);

// 0:false, 1:true
struct OpStat {
  cache::AcqRelAtomic<int64_t> insert_status[2];
  cache::AcqRelAtomic<int64_t> get_status[2];

  OpStat() {
    insert_status[0].StoreRelaxed(0);
    insert_status[1].StoreRelaxed(0);

    get_status[0].StoreRelaxed(0);
    get_status[1].StoreRelaxed(0);
  }

  std::string Print() {
    std::ostringstream oss;
    oss << "[Insert] fail=" << insert_status[0].LoadRelaxed()
        << ", succ=" << insert_status[1].LoadRelaxed()
        << " | [Get] fail=" << get_status[0].LoadRelaxed()
        << ", succ=" << get_status[1].LoadRelaxed();
    return oss.str();
  }
};

OpStat lru_stat;
OpStat hs_stat;
OpStat clock_stat;

const bool enable_stat = false;

void CalcClockCacheDisplacements(const cache::clock::Cache &clock_cache) {
  std::vector<size_t> displacements;
  displacements.reserve(clock_cache.GetLength());
  int64_t displacements_sum = 0;
  int32_t displacements_max = 0;
  std::priority_queue<int32_t> displacements_queue;
  for (const auto &shard : clock_cache.GetShards()) {
    for (size_t i = 0; i < shard->GetLength(); i++) {
      int32_t displacement = shard->HandlePtr(i)->displacements.LoadRelaxed();
      displacements.push_back(displacement);
      displacements_sum += displacement;
      displacements_max = std::max(displacement, displacements_max);

      displacements_queue.push(displacement);
    }
  }
  double average = displacements_sum * 1.0 / clock_cache.GetLength();

  double average_top_10000 = 0;
  for (int32_t i = 0; i < 10000; i++) {
    average_top_10000 += displacements_queue.top();
    displacements_queue.pop();
  }
  average_top_10000 /= 10000;

  std::cout << " average: " << average << ", max: " << displacements_max
            << ", average_top_10000: " << average_top_10000 << std::endl;
}

static void BenchmarkLRUCacheReadWrite(benchmark::State &state) {
  const int64_t max_num = state.range(0);
  const double miss_ratio = state.range(1) * 1.0 / 100;
  for (auto s : state) {
    int64_t key = rnd() * max_num;
    cache::lru::Handle handle;

    if (lru_cache.Get(key, &handle)) {
      if (enable_stat) {
        const bool succ = *handle.value.get() == key;
        lru_stat.get_status[succ].FetchAddRelaxed(1);
      }
    }
    benchmark::DoNotOptimize(handle);

    if (cache::FastRandomDouble() < miss_ratio) {
      // if (butil::fast_rand_double() < miss_ratio) {
      handle.value = std::make_shared<int64_t>(key);
      handle.total_charge = 16;
      lru_cache.Insert(key, handle);
      if (enable_stat) {
        lru_stat.insert_status[true].FetchAddRelaxed(1);
      }
    }
  }
}

static void BenchmarkHSCacheReadWrite(benchmark::State &state) {
  const int64_t max_num = state.range(0);
  const double miss_ratio = state.range(1) * 1.0 / 100;
  for (auto s : state) {
    int64_t key = rnd() * max_num;

    auto handle = hs_cache.Get(key);
    if (handle) {
      if (enable_stat) {
        const bool succ =
            std::static_pointer_cast<Handle>(handle)->Val() == key;
        hs_stat.get_status[succ].FetchAddRelaxed(1);
      }
    }
    benchmark::DoNotOptimize(handle);

    if (cache::FastRandomDouble() < miss_ratio) {
      auto handle = std::make_shared<Handle>(key);
      bool succ = hs_cache.Insert(key, handle);
      if (enable_stat) {
        hs_stat.insert_status[succ].FetchAddRelaxed(1);
      }
    }
  }
}

static void BenchmarkClockCacheReadWrite(benchmark::State &state) {
  const int64_t max_num = state.range(0);
  const double miss_ratio = state.range(1) * 1.0 / 100;
  cache::clock::DeleterFn del_cb = [](cache::clock::ObjectPtr obj) {
    delete static_cast<int64_t *>(obj);
  };
  for (auto s : state) {
    int64_t key = rnd() * max_num;

    auto handle = clock_cache.Lookup(&key, sizeof(key));
    if (handle) {
      if (enable_stat) {
        const bool succ = *static_cast<int64_t *>(handle->value) == key;
        clock_stat.get_status[succ].FetchAddRelaxed(1);
      }
      clock_cache.Release(handle);
    }
    benchmark::DoNotOptimize(handle);

    if (cache::FastRandomDouble() < miss_ratio) {
      cache::clock::Handle handle;
      handle.value = new int64_t(key);
      handle.del_cb = del_cb;
      handle.total_charge = 16;

      int retry = 3;
      bool succ = false;
      do {
        succ = clock_cache.Insert(&key, sizeof(key), handle);
      } while (!succ && --retry > 0);

      if (enable_stat) {
        clock_stat.insert_status[succ].FetchAddRelaxed(1);
      }
    }
  }
}

int main(int argc, char *argv[]) {
  if (!hs_cache.Init()) {
    std::cout << __LINE__ << " " << __FUNCTION__ << " lockfree cache init fail"
              << std::endl;
    return -1;
  }

  benchmark::Initialize(&argc, argv);
  const int64_t max_num = 10000 * 10000;
  const int64_t miss_ratio = 20;
  const int thread_num = 16;
  const int32_t min_time = 10;
  benchmark::RegisterBenchmark("LRUCacheReadWrite", BenchmarkLRUCacheReadWrite)
      ->Args({max_num, miss_ratio})
      ->ArgNames({"range", "miss_ratio"})
      ->MinTime(min_time)
      ->Threads(thread_num);
  benchmark::RegisterBenchmark("HSCacheReadWrite", BenchmarkHSCacheReadWrite)
      ->Args({max_num, miss_ratio})
      ->ArgNames({"range", "miss_ratio"})
      ->MinTime(min_time)
      ->Threads(thread_num);
  benchmark::RegisterBenchmark("ClockCacheReadWrite",
                               BenchmarkClockCacheReadWrite)
      ->Args({max_num, miss_ratio})
      ->ArgNames({"range", "miss_ratio"})
      ->MinTime(min_time)
      ->Threads(thread_num);
  benchmark::RunSpecifiedBenchmarks();

  if (enable_stat) {
    std::cout << "lru " << lru_stat.Print() << std::endl;
    std::cout << "hs " << hs_stat.Print()
              << " | low queue:" << hs_cache.GetLowQueueSize() << std::endl;
    std::cout << "clock " << clock_stat.Print()
              << " | clock_cahce usage:" << clock_cache.GetUsage()
              << ", capicity:" << capicity
              << ", occupancy:" << clock_cache.GetOccupancy()
              << ", occupancy_limit:" << clock_cache.GetOccupancyLimit()
              << ", length:" << clock_cache.GetLength() << std::endl;
    CalcClockCacheDisplacements(clock_cache);
  }
  return 0;
}
