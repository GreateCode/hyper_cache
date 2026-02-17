#include "clock/clock_cache.h"

#include <assert.h>
#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>

#include "clock/handle.h"
#include "util/murmur128_hash.h"
#include "util/util.h"

namespace hyper_cache::clock {

namespace {

class PinGuard {
public:
    PinGuard(ClockCache* cache) : cache_(cache), pin_id_(cache->Pin()) {}
    ~PinGuard() {
        if (cache_) {
            cache_->Unpin(pin_id_);
        }
    }

private:
    ClockCache* cache_{nullptr};
    int32_t pin_id_{-1};
};

thread_local uintptr_t g_ref_count_stripe_token = 0;

// Mix bits for better distribution when token is sequential (e.g. bthread id). K=64.
constexpr int kStripeMask = 63;
inline uintptr_t StripeIndex(uintptr_t token) {
    uintptr_t h = token;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return h & kStripeMask;
}

}  // namespace

void ClockCache::SetRefCountStripeToken(uintptr_t token) { g_ref_count_stripe_token = token; }

uintptr_t ClockCache::GetRefCountStripeToken() {
    uintptr_t t = g_ref_count_stripe_token;
    if (t != 0) {
        return t;
    }
    return static_cast<uintptr_t>(reinterpret_cast<uintptr_t>(pthread_self()));
}

int32_t ClockCache::Pin() {
    uintptr_t i = StripeIndex(GetRefCountStripeToken());
    ref_count_stripes_[i].count.FetchAddAcqRel(1);
    return static_cast<int32_t>(i);
}

void ClockCache::Unpin(int32_t id) {
    if (id < 0 || id >= kRefCountStripes) {
        return;
    }
    ref_count_stripes_[id].count.FetchSubAcqRel(1);
}

int64_t ClockCache::PinCount() const {
    int64_t sum = 0;
    for (int i = 0; i < kRefCountStripes; ++i) {
        sum += ref_count_stripes_[i].count.LoadRelaxed();
    }
    std::atomic_thread_fence(std::memory_order_acquire);
    return sum;
}

const int64_t kInitHitCount         = 3;
const int64_t kMaxHitCount          = 2000;
const int64_t kDecrHitPerEntryCount = 5000;

inline static uint64_t RefIncr(ClockHandle* h) { return h->meta.FetchAddAcqRel(ClockHandle::kRefIncrement); }

inline static void RefDecr(ClockHandle* h) { h->meta.FetchSubAcqRel(ClockHandle::kRefIncrement); }

inline static int64_t RefCount(uint64_t meta) {
    return (meta >> ClockHandle::kRefCounterShift) & ClockHandle::kRefCounterMask;
}

inline static int64_t HitCount(uint64_t meta) {
    return (meta >> ClockHandle::kHitCounterShift) & ClockHandle::kHitCounterMask;
}

inline static void HitDecr(ClockHandle* h, int64_t count = 1) {
    h->meta.FetchSubAcqRel(ClockHandle::kHitIncrement * count);
}

inline static void HitIncr(ClockHandle* h, int64_t count = 1) {
    const uint64_t old_meta = h->meta.FetchAddAcqRel(ClockHandle::kHitIncrement * count);
    const int64_t overflow  = HitCount(old_meta) + count - kMaxHitCount;
    if (overflow > 0) {
        HitDecr(h, overflow);
    }
}

inline static int64_t GetMetaId(uint64_t meta) { return (meta >> ClockHandle::kIdShift) & ClockHandle::kIdMask; }

inline static void SetMetaId(ClockHandle* h, int64_t id = 0) { h->meta.StoreRelease(ClockHandle::kIdIncrement * id); }

UniqueId64x2 DefaultHashKeyFn(const void* key, int32_t key_size, uint32_t seed) {
    uint64_t h1 = 0, h2 = 0;
    MurmurHash3_x64_128(key, key_size, seed, &h1, &h2);
    return UniqueId64x2{h1, h2};
}

inline static void MarkEmpty(ClockHandle* h) {
    // Mark slot as empty, with assertion
    h->meta.ExchangeAcqRel(0);
}

inline static void FreeDataMarkEmpty(ClockHandle* h) {
    h->FreeData();
    MarkEmpty(h);
}

inline static bool BeginSlotInsert(const Handle& handle, ClockHandle* h, bool* already_matches) {
    assert(*already_matches == false);
    uint64_t old_meta  = h->meta.FetchOrAcqRel(uint64_t{ClockHandle::kStateOccupiedBit} << ClockHandle::kStateShift);
    uint64_t old_state = old_meta >> ClockHandle::kStateShift;

    if (old_state == ClockHandle::kStateEmpty) {
        return true;
    } else if (old_state != ClockHandle::kStateVisible) {
        return false;
    }

    old_meta = RefIncr(h);
    // Like Lookup
    if ((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateVisible) {
        if (h->hashed_key == handle.hashed_key) {
            RefDecr(h);
            *already_matches = true;
            // because rocksdb cache are immutable sst blocks, so we don't need to
            // update the key's value, but the general cache needs to update
            return false;
        } else {
            RefDecr(h);
        }
    } else if (unlikely((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateInvisible)) {
        RefDecr(h);
    }

    return false;
}

inline static void FinishSlotInsert(const Handle& handle, ClockHandle* h) {
    // Save data fields
    Handle* h_alias = h;
    *h_alias        = handle;

    uint64_t new_meta = uint64_t{ClockHandle::kStateVisible} << ClockHandle::kStateShift;

    new_meta |= kInitHitCount << ClockHandle::kHitCounterShift;

    uint64_t old_meta = h->meta.ExchangeAcqRel(new_meta);
    assert(old_meta >> ClockHandle::kStateShift == ClockHandle::kStateConstruction);
}

inline static bool TryInsert(const Handle& handle, ClockHandle* h, bool* already_matches) {
    const bool b = BeginSlotInsert(handle, h, already_matches);
    if (b) {
        FinishSlotInsert(handle, h);
    }
    return b;
}

inline static bool IsEvictionEffortExceeded(const ClockCache::EvictionData& data, uint32_t eviction_effort_cap = 30) {
    return (data.freed_count + 1U) * uint64_t{eviction_effort_cap} <= data.seen_pinned_count;
}

inline static bool ClockUpdate(ClockHandle* h, ClockCache::EvictionData* data, int64_t* hit_count_sum,
                               int32_t hit_decr_delta) {
    uint64_t meta = h->meta.LoadRelaxed();

    if (((meta >> ClockHandle::kStateShift) & ClockHandle::kStateShareableBit) == 0) {
        return false;
    }
    if (RefCount(meta) > 0) {
        data->seen_pinned_count++;
        return false;
    }

    const int64_t hit_count = HitCount(meta);
    if ((meta >> ClockHandle::kStateShift == ClockHandle::kStateVisible) && hit_count > 0) {
        int64_t new_hit_count = hit_count;
        new_hit_count         = std::max(new_hit_count - hit_decr_delta, 0ll);

        const uint64_t new_meta = (uint64_t{ClockHandle::kStateVisible} << ClockHandle::kStateShift) |
                                  (meta & ClockHandle::kHitBitMask) | (new_hit_count << ClockHandle::kHitCounterShift);
        h->meta.CasStrongRelaxed(meta, new_meta);
        hit_count_sum += new_hit_count;
        return false;
    }
    if (h->meta.CasStrongAcqRel(meta, (uint64_t{ClockHandle::kStateConstruction} << ClockHandle::kStateShift) |
                                          (meta & ClockHandle::kHitBitMask))) {
        // Took ownership.
        data->freed_charge += h->GetTotalCharge();
        data->freed_count += 1;
        return true;
    } else {
        return false;
    }
}

ClockCache::ClockCache(const ClockCacheOptions& options)
    : capacity_(options.capacity),
      length_(options.length > 0 ? options.length
                                 : (options.value_size > 0 ? (options.capacity / options.value_size) : 0)),
      occupancy_limit_(options.length * options.load_factor),
      hash_key_fn_(options.hash_key_fn),
      hit_decr_delta_min_(options.hit_decr_delta_min),
      array_(std::make_unique<HandleImpl[]>(options.length)),
      options_(options) {}

ClockCache::~ClockCache() {
    for (size_t i = 0; i < GetLength(); i++) {
        HandleImpl& h = array_[i];
        switch (h.meta.LoadRelaxed() >> ClockHandle::kStateShift) {
            case ClockHandle::kStateEmpty:
                // noop
                break;
            case ClockHandle::kStateInvisible:  // rare but possible
            case ClockHandle::kStateVisible:
                assert(Refcount(h.meta.LoadRelaxed()) == 0);
                h.FreeData();
                Rollback(h.hashed_key, &h);
                ReclaimEntryUsage(h.GetTotalCharge());
                break;
            // otherwise
            default:
                assert(false);
                break;
        }
    }

    for (size_t i = 0; i < GetLength(); i++) {
        assert(array_[i].displacements.LoadRelaxed() == 0);
    }

    assert(usage_.LoadRelaxed() == 0 || usage_.LoadRelaxed() == size_t{GetLength()} * sizeof(HandleImpl));
    assert(occupancy_.LoadRelaxed() == 0);
}

bool ClockCache::Release(HandlePin* handle_pin) {
    Release(handle_pin->handle);
    Unpin(handle_pin->pin_id);
    return true;
}

bool ClockCache::Release(HandleImpl* handle) {
    if (handle) {
        RefDecr(handle);
    }
    return true;
}

bool ClockCache::Lookup(const UniqueId64x2& hashed_key, HandlePin* handle_pin) {
    handle_pin->pin_id = Pin();
    handle_pin->handle = Lookup(hashed_key);
    if (handle_pin->handle == nullptr) {
        Unpin(handle_pin->pin_id);
        return false;
    }
    return true;
}

HandleImpl* ClockCache::Lookup(const UniqueId64x2& hashed_key) {
    HandleImpl* e = FindSlot(
        hashed_key,
        [&](HandleImpl* h) {
            // increment acquire counter
            const uint64_t old_meta = RefIncr(h);
            // Check if it's an entry visible to lookups
            if ((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateVisible) {
                // Acquired a read reference
                if (h->hashed_key == hashed_key) {
                    // Match
                    HitIncr(h);
                    return true;
                } else {
                    // Mismatch
                    RefDecr(h);
                }
            } else if (unlikely((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateInvisible)) {
                RefDecr(h);
            } else {
            }
            return false;
        },
        [&](HandleImpl* h) { return h->displacements.LoadRelaxed() == 0; },
        [&](HandleImpl* /*h*/, bool /*is_last*/) {});

    return e;
}

bool ClockCache::Insert(const Handle& handle) {
    PinGuard pin_guard(this);

    const size_t old_occupancy          = occupancy_.FetchAddAcqRel(1);
    const bool need_evict_for_occupancy = old_occupancy + 1 > occupancy_limit_;

    if (!EvictForCharge(handle.GetTotalCharge(), need_evict_for_occupancy)) {
        occupancy_.FetchSubRelaxed(1);
        return false;
    }

    bool already_matches = false;
    // new
    if (DoInsert(handle, &already_matches)) {
        return true;
    }
    // update
    if (already_matches && Erase(handle.hashed_key) && DoInsert(handle, &already_matches)) {
        return true;
    }

    occupancy_.FetchSubRelaxed(1);
    usage_.FetchSubRelaxed(handle.GetTotalCharge());
    return false;
}

bool ClockCache::Erase(const UniqueId64x2& hashed_key) {
    PinGuard pin_guard(this);
    bool found = false;
    bool succ  = false;
    (void)FindSlot(
        hashed_key,
        [&](HandleImpl* h) {
            uint64_t old_meta = RefIncr(h);
            // Check if it's an entry visible to lookups
            if ((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateVisible) {
                if (h->hashed_key == hashed_key) {
                    found = true;
                    // Match. Set invisible.
                    old_meta =
                        h->meta.FetchAndAcqRel(~(uint64_t{ClockHandle::kStateVisibleBit} << ClockHandle::kStateShift));
                    old_meta &= ~(uint64_t{ClockHandle::kStateVisibleBit} << ClockHandle::kStateShift);
                    for (;;) {
                        if (RefCount(old_meta) > 1) {  // this func has added 1
                            RefDecr(h);
                            break;
                        } else if (h->meta.CasWeakAcqRel(old_meta, uint64_t{ClockHandle::kStateConstruction}
                                                                       << ClockHandle::kStateShift)) {
                            // Took ownership
                            assert(hashed_key == h->hashed_key);
                            const size_t total_charge = h->GetTotalCharge();
                            FreeDataMarkEmpty(h);
                            ReclaimEntryUsage(total_charge);
                            Rollback(hashed_key, h);
                            succ = true;
                            break;
                        }
                    }
                } else {
                    RefDecr(h);
                }
            } else if (unlikely((old_meta >> ClockHandle::kStateShift) == ClockHandle::kStateInvisible)) {
                RefDecr(h);
            } else {
                RefDecr(h);
            }
            return false;
        },
        [&](HandleImpl* h) { return h->displacements.LoadRelaxed() == 0; },
        [&](HandleImpl* /*h*/, bool /*is_last*/) {});
    return succ || !found;  // cache中不存在即成功
}

bool ClockCache::TryPickHandle(size_t idx, Handle* handle, bool* has_handle) {
    HandleImpl& h       = array_[idx];
    uint64_t meta       = h.meta.LoadAcquire();
    const uint8_t state = meta >> ClockHandle::kStateShift;
    if (state != ClockHandle::kStateVisible) {
        *has_handle = false;
        return false;
    }

    *has_handle = true;

    if (RefCount(meta) > 0) {
        return false;
    }

    if (!h.meta.CasStrongAcqRel(meta, (uint64_t{ClockHandle::kStateConstruction} << ClockHandle::kStateShift) |
                                          (meta & ClockHandle::kHitBitMask))) {
        return false;
    }

    Rollback(h.hashed_key, &h);

    if (handle) {
        handle->value        = h.value;
        handle->del_cb       = h.del_cb;
        handle->hashed_key   = h.hashed_key;
        handle->total_charge = h.total_charge;
    }
    h.value  = nullptr;
    h.del_cb = nullptr;
    MarkEmpty(&h);

    ReclaimEntryUsage(handle ? handle->GetTotalCharge() : h.GetTotalCharge());
    return true;
}

void ClockCache::Evict(size_t requested_charge, EvictionData* data) {
    // precondition
    assert(requested_charge > 0);

    constexpr size_t step_size = 4;

    // First (concurrent) increment clock pointer
    uint64_t old_clock_pointer = clock_pointer_.FetchAddRelaxed(step_size);
    uint64_t max_clock_pointer = old_clock_pointer + (ClockHandle::kMaxCountdown * length_);
    int64_t offset             = 0;

    int64_t hit_count_sum  = 0;
    int32_t hit_decr_delta = hit_decr_delta_.LoadRelaxed();
    std::shared_ptr<void> _(nullptr, std::bind([&]() { hit_decr_delta_.StoreRelaxed(hit_decr_delta); }));
    for (;;) {
        for (size_t i = 0; i < step_size; i++) {
            HandleImpl& h = array_[ModLength(old_clock_pointer + i)];
            if (ClockUpdate(&h, data, &hit_count_sum, hit_decr_delta)) {
                Rollback(h.hashed_key, &h);
                TrackAndReleaseEvictedEntry(&h);
            }
        }

        if (data->freed_charge >= requested_charge) {
            return;
        }
        if (old_clock_pointer >= max_clock_pointer) {
            return;
        }
        if (IsEvictionEffortExceeded(*data)) {
            return;
        }

        old_clock_pointer = clock_pointer_.FetchAddRelaxed(step_size);
        offset += step_size;
        if (offset + step_size >= kDecrHitPerEntryCount) {
            hit_decr_delta = std::max(hit_count_sum * 1.0 / kDecrHitPerEntryCount, 1.0);
            if (hit_decr_delta < hit_decr_delta_min_) {
                hit_decr_delta = hit_decr_delta_min_;
            }
            hit_count_sum = 0;
        }
    }
}

void ClockCache::Rollback(const UniqueId64x2& hashed_key, const HandleImpl* h) {
    size_t current         = ModLength(hashed_key[1]);
    const size_t increment = static_cast<size_t>(hashed_key[0]) | 1U;
    while (&array_[current] != h) {
        array_[current].displacements.FetchSubRelaxed(1);
        current = ModLength(current + increment);
    }
}

void ClockCache::ReclaimEntryUsage(size_t total_charge) {
    auto old_occupancy = occupancy_.FetchSubAcqRel(1U);
    (void)old_occupancy;
    auto old_usage = usage_.FetchSubRelaxed(total_charge);
    (void)old_usage;
}

template <typename MatchFn, typename AbortFn, typename UpdateFn>
HandleImpl* ClockCache::FindSlot(const UniqueId64x2& hashed_key, const MatchFn& match_fn, const AbortFn& abort_fn,
                                 const UpdateFn& update_fn) {
    size_t base      = static_cast<size_t>(hashed_key[1]);
    size_t increment = static_cast<size_t>(hashed_key[0]) | 1U;
    size_t first     = ModLength(base);
    size_t current   = first;
    bool is_last;
    do {
        HandleImpl* h = &array_[current];
        if (match_fn(h)) {
            return h;
        }
        if (abort_fn(h)) {
            return nullptr;
        }
        current = ModLength(current + increment);
        is_last = current == first;
        update_fn(h, is_last);
    } while (!is_last);
    return nullptr;
}

HandleImpl* ClockCache::DoInsert(const Handle& handle, bool* already_matches) {
    *already_matches = false;
    HandleImpl* e    = FindSlot(
        handle.hashed_key,
        [&](HandleImpl* h) {
            SetMetaId(h, GetId());
            return TryInsert(handle, h, already_matches);
        },
        [&](HandleImpl* h) {
            if (*already_matches) {
                // Stop searching & roll back displacements
                Rollback(handle.hashed_key, h);
                return true;
            } else {
                // Keep going
                return false;
            }
        },
        [&](HandleImpl* h, bool is_last) {
            if (is_last) {
                // Search is ending. Roll back displacements
                Rollback(handle.hashed_key, h);
            } else {
                h->displacements.FetchAddRelaxed(1);
            }
        });
    if (*already_matches) {
        // Insertion skipped
        return nullptr;
    }
    if (e != nullptr) {
        // Successfully inserted
        return e;
    }

    return nullptr;
}

bool ClockCache::EvictForCharge(size_t total_charge, bool need_evict_for_occupancy) {
    size_t old_usage = usage_.LoadRelaxed();
    size_t new_usage;
    do {
        new_usage = std::min(capacity_, old_usage + total_charge);
        if (new_usage == old_usage) {
            break;
        }
    } while (!usage_.CasWeakRelaxed(old_usage, new_usage));

    const size_t need_evict_charge = old_usage + total_charge - new_usage;
    size_t request_evict_charge    = need_evict_charge;
    if (unlikely(need_evict_for_occupancy) && request_evict_charge == 0) {
        request_evict_charge = 1;
    }
    if (request_evict_charge > 0) {
        EvictionData data;
        Evict(request_evict_charge, &data);
        occupancy_.FetchSubAcqRel(data.freed_count);
        if (likely(data.freed_charge > need_evict_charge)) {
            assert(data.freed_count > 0);
            usage_.FetchSubRelaxed(data.freed_charge - need_evict_charge);
        } else if (data.freed_charge < need_evict_charge ||
                   (unlikely(need_evict_for_occupancy) && data.freed_count == 0)) {
            usage_.FetchSubRelaxed(data.freed_charge + (new_usage - old_usage));
            return false;
        }
        assert(data.freed_count > 0);
    }
    return true;
}

void ClockCache::TrackAndReleaseEvictedEntry(ClockHandle* h) { FreeDataMarkEmpty(h); }

}  // namespace hyper_cache::clock
