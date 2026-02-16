#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <memory>

// #include "/root/workdir/opt/include/xxhash.h"
#include "clock/handle.h"
#include "util/atomic.h"

namespace hyper_cache::clock {

// hash
using HashKeyFnType = UniqueId64x2 (*)(const void* key, int32_t key_size, uint32_t seed);

// suggest: https://github.com/Cyan4973/xxHash
UniqueId64x2 DefaultHashKeyFn(const void* key, int32_t key_size, uint32_t seed);

struct ClockCacheOptions {
    size_t capacity    = 0;     // total capacity of the cache
    size_t length      = 0;     // number of slots in the cache, suggest 2^n
    size_t value_size  = 0;     // only set one of length , value_size.
    double load_factor = 0.65;  // load factor of the cache

    HashKeyFnType hash_key_fn = DefaultHashKeyFn;

    // use for accelerate the hit count decrease speed, when the hit count is too
    // large, the decrease speed will be too slow
    int32_t hit_decr_delta_min = 1;
};

class ClockCache {
public:
    struct EvictionData {
        size_t freed_charge      = 0;
        size_t freed_count       = 0;
        size_t seen_pinned_count = 0;
    };

public:
    ClockCache(const ClockCacheOptions& options);
    ~ClockCache();

    HandleImpl* Lookup(const UniqueId64x2& hash_key);
    bool Insert(const Handle& handle);

    size_t GetCapacity() const { return capacity_; }

    size_t GetOccupancy() const { return occupancy_.LoadRelaxed(); }

    size_t GetUsage() const { return usage_.LoadRelaxed(); }

    bool Release(HandleImpl* handle);

    bool Erase(const UniqueId64x2& hashed_key);

    size_t GetLength() const { return length_; }

    size_t GetOccupancyLimit() const { return occupancy_limit_; }

    const HandleImpl* HandlePtr(size_t idx) const { return &array_[idx]; }

    bool TryPickHandle(size_t idx, Handle* handle, bool* has_handle);

    uint8_t GetId() const { return id_; }
    uint8_t SetId(uint8_t id) { return id_ = id; }
    const ClockCacheOptions& Options() const { return options_; }
    bool IsScaling() const { return scaling_.LoadAcquire(); }
    void SetScaling(bool scaling) { scaling_.StoreRelease(scaling); }
    bool IsRetired() const { return is_retired_.LoadAcquire(); }
    void SetRetired(bool retired) { is_retired_.StoreRelease(retired); }

    // Striped ref count: Pin before use, Unpin when done. Scale waits for GetRefCount()==0 before delete.
    void Pin();
    void Unpin();
    int64_t PinCount() const;

    // Optional: set per-bthread token for better stripe distribution (e.g. bthread_self()). Defaults to pthread_self().
    static void SetRefCountStripeToken(uintptr_t token);
    static uintptr_t GetRefCountStripeToken();

private:
    static constexpr int kRefCountStripes    = 64;  // power of 2 for (token & (kRefCountStripes-1))
    static constexpr int kRefCountStripeMask = kRefCountStripes - 1;
    struct alignas(64) RefCountStripe {
        AcqRelAtomic<int64_t> count{0};
    };
    RefCountStripe ref_count_stripes_[kRefCountStripes];

private:
    void Evict(size_t requested_charge, EvictionData* data);

    inline size_t ModLength(uint64_t x) { return x % length_; }

    template <typename MatchFn, typename AbortFn, typename UpdateFn>
    HandleImpl* FindSlot(const UniqueId64x2& hashed_key, const MatchFn& match_fn, const AbortFn& abort_fn,
                         const UpdateFn& update_fn);

    void Rollback(const UniqueId64x2& hashed_key, const HandleImpl* h);

    HandleImpl* DoInsert(const Handle& handle, bool* already_matches);

    void ReclaimEntryUsage(size_t total_charge);

    bool EvictForCharge(size_t total_charge, bool need_evict_for_occupancy);

    void TrackAndReleaseEvictedEntry(ClockHandle* h);

private:
    uint8_t id_{0};
    RelaxedAtomic<uint64_t> clock_pointer_{0};
    AcqRelAtomic<size_t> occupancy_{0};
    AcqRelAtomic<size_t> usage_{0};
    AcqRelAtomic<int32_t> hit_decr_delta_{1};
    const int32_t hit_decr_delta_min_ = 1;

    const uint32_t hash_seed_ = -2;

    const size_t capacity_;
    const size_t length_;
    const size_t occupancy_limit_;
    const HashKeyFnType hash_key_fn_;
    const double load_factor_ = 0.65;
    const std::unique_ptr<HandleImpl[]> array_;
    const ClockCacheOptions options_;
    AcqRelAtomic<bool> scaling_{false};
    AcqRelAtomic<bool> is_retired_{false};
};

}  // namespace hyper_cache::clock
