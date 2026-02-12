#pragma once

#include <cstdint>
#include <memory>

#include "hash_skip/atomic_shared_ptr.h"
#include "hash_skip/ptr_util.h"
#include "util/atomic.h"

namespace hyper_cache::hash_skip {

const int16_t MAX_HIT_COUNT = 1000;

class Handle {  // for erase type
public:
    virtual ~Handle()              = default;
    virtual int32_t Charge() const = 0;
};

class Node {
public:
    Node(int64_t key, std::shared_ptr<Handle> handle, int8_t top_level)
        : key(key), top_level(top_level), handle_(handle) {
        nexts = std::make_unique<AcqRelAtomic<MarkVersionedPtr>[]>(top_level + 1);
        for (int8_t i = 0; i <= top_level; ++i) {
            // initial pointer=nullptr, version=0, mark=0
            nexts[i].StoreRelaxed(nullptr);
        }
    }

    ~Node() {}

    bool IsMarked() { return nexts[0].LoadAcquire().IsMarked(); }

    void IncrHitCount() {
        int16_t old_count = hit_count_.LoadAcquire();
        int16_t new_count = 0;
        do {
            if (old_count >= MAX_HIT_COUNT) {
                return;  // reached the upper limit, no need to increase
            }
            new_count = old_count + 1;
        } while (!hit_count_.CasWeakRelease(old_count, new_count));
    }

    int16_t DecrHitCount(int16_t delta) {
        int16_t old_count = hit_count_.LoadAcquire();
        int16_t new_count = 0;
        do {
            if (old_count <= 0) {
                return 0;  // reached the lower limit, no need to decrease
            }
            new_count = old_count <= delta ? 0 : old_count - delta;
        } while (!hit_count_.CasWeakRelease(old_count, new_count));
        return new_count;
    }

    int32_t MarkDeleted() {
        MarkVersionedPtr expected = nexts[0].LoadAcquire();
        while (true) {
            if (expected.IsMarked()) {
                return 0;  // already marked
            }
            const MarkVersionedPtr desired =
                MarkVersionedPtr::PackPtr(expected.GetRawPtr(), expected.GetVersion() + 1, true);
            if (!nexts[0].CasWeakAcqRel(expected, desired)) {
                continue;
            }

            // only one thread can execute here, other threads failed because it is
            // already marked
            std::shared_ptr<Handle> old_handle = handle_.Store(nullptr);
            return old_handle ? old_handle->Charge() : 0;
        }
    }

    // Lock-free get and update handle
    std::shared_ptr<Handle> GetHandle() { return handle_.Load(); }
    std::shared_ptr<Handle> UpdateHandle(std::shared_ptr<Handle> h) { return handle_.Store(std::move(h)); }

public:
    int64_t key{0};
    std::unique_ptr<AcqRelAtomic<MarkVersionedPtr>[]> nexts{
        nullptr};  // for skiplist dynamic allocation, length top_level_+1

    int8_t top_level{0};

private:
    AtomicSharedPtr<Handle> handle_{nullptr};  // std::shared_ptr<Handle> handle{nullptr};
    AcqRelAtomic<int16_t> hit_count_{200};     // access count, initial value to avoid being immediately put into
                                               // coldlist, if cold will decay
};

}  // namespace hyper_cache::hash_skip
