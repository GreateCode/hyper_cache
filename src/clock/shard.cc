#include <clock/shard.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <thread>

#include "clock/clock_cache.h"
#include "clock/handle.h"

namespace hyper_cache::clock {

ShardWrapper::ShardWrapper(ClockCacheOptions options) {
    ClockCache* cc = new ClockCache(options);
    cc->SetId(0);

    replicas_[0].StoreRelease(cc);

    current_idx_.StoreRelease(0);
    retired_ref_count_.StoreRelease(0);
}

ShardWrapper::~ShardWrapper() {
    for (int32_t i = 0; i < replicas_.size(); ++i) {
        ClockCache* cc = replicas_[i].LoadAcquire();
        if (!cc) {
            continue;
        }
        if (scaling_.LoadAcquire() && (1 - current_idx_.LoadAcquire()) == i) {
            assert(retired_ref_count_.LoadAcquire() == 0);
        }
        delete cc;
        replicas_[i].StoreRelease(nullptr);
    }
}

HandleImpl* ShardWrapper::Lookup(const UniqueId64x2& hashed_key) {
    ClockCache* current = AcquireCurrent();
    if (current) {
        HandleImpl* handle = current->Lookup(hashed_key);
        if (handle) {
            return handle;
        }
    }

    if (!scaling_.LoadAcquire()) {
        return nullptr;
    }

    retired_ref_count_.FetchAddAcqRel(1);
    ClockCache* retired = AcquireRetired();
    if (!retired) {
        retired_ref_count_.FetchSubAcqRel(1);
    }
    HandleImpl* handle = retired->Lookup(hashed_key);
    if (!handle) {
        retired_ref_count_.FetchSubAcqRel(1);
        return nullptr;
    }
    return handle;
}

bool ShardWrapper::Insert(const Handle& handle) {
    ClockCache* current = AcquireCurrent();
    if (!current) {
        return false;
    }
    return current->Insert(handle);
}

bool ShardWrapper::Release(HandleImpl* handle) {
    if (!scaling_.LoadAcquire()) {
        ClockCache* current = AcquireCurrent();
        if (current) {
            return current->Release(handle);
        }
    }

    uint8_t id = handle->Id();
    for (int i = 0; i < replicas_.size(); ++i) {
        if (!replicas_[i].LoadAcquire()) {
            continue;
        }
        ClockCache* replica = replicas_[i].LoadAcquire();
        if (!replica) {
            continue;
        }
        if (id == replica->GetId()) {
            replica->Release(handle);
            if (replica->IsRetired()) {
                retired_ref_count_.FetchSubAcqRel(1);
            }
            return true;
        }
    }
    return false;
}

bool ShardWrapper::Erase(const UniqueId64x2& hashed_key) {
    bool succ           = false;
    ClockCache* current = AcquireCurrent();
    if (current) {
        succ = current->Erase(hashed_key);
        if (!scaling_.LoadAcquire()) {
            return succ;
        }
    }

    retired_ref_count_.FetchAddAcqRel(1);
    ClockCache* retired = AcquireRetired();
    if (!retired) {
        retired_ref_count_.FetchSubAcqRel(1);
        return succ;
    }
    succ &= retired->Erase(hashed_key);
    retired_ref_count_.FetchSubAcqRel(1);
    return succ;
}

const HandleImpl* ShardWrapper::HandlePtr(size_t idx) const {
    const ClockCache* current = AcquireCurrent();
    if (current && idx < current->GetLength()) {
        return current->HandlePtr(idx);
    }
    if (!scaling_.LoadAcquire()) {
        return nullptr;
    }
    const ClockCache* retired = AcquireRetired();
    if (retired && idx < retired->GetLength()) {
        return retired->HandlePtr(idx);
    }
    return nullptr;
}

size_t ShardWrapper::GetCapacity() const {
    const ClockCache* current = AcquireCurrent();
    return current ? current->GetCapacity() : 0;
}

size_t ShardWrapper::GetOccupancy() const {
    const ClockCache* current = AcquireCurrent();
    return current ? current->GetOccupancy() : 0;
}

size_t ShardWrapper::GetUsage() const {
    const ClockCache* current = AcquireCurrent();
    return current ? current->GetUsage() : 0;
}

size_t ShardWrapper::GetLength() const {
    const ClockCache* current = AcquireCurrent();
    return current ? current->GetLength() : 0;
}

size_t ShardWrapper::GetOccupancyLimit() const {
    const ClockCache* current = AcquireCurrent();
    return current ? current->GetOccupancyLimit() : 0;
}

ClockCache* ShardWrapper::AcquireCurrent() const { return replicas_[current_idx_.LoadAcquire()].LoadAcquire(); }

ClockCache* ShardWrapper::AcquireRetired() const { return replicas_[1 - current_idx_.LoadAcquire()].LoadAcquire(); }

void ShardWrapper::SmoothScale() {
    ClockCache* current = AcquireCurrent();
    if (!current) {
        return;
    }
    ClockCacheOptions options = current->Options();
    if (current->GetOccupancy() * 1.0 / current->GetLength() < options.load_factor * 0.9 ||
        current->GetUsage() > options.capacity * 0.9) {
        return;  // no need to scale
    }

    bool expect = false;
    if (!scaling_.CasStrongAcqRel(expect, true)) {
        return;
    }

    const int32_t retired_idx = 1 - current_idx_.LoadAcquire();
    ClockCache* retired       = replicas_[retired_idx].LoadAcquire();
    if (retired) {
        delete retired;
        retired = nullptr;
        replicas_[retired_idx].StoreRelease(nullptr);
    }

    options.length *= 2;
    ClockCache* new_current = new ClockCache(options);
    new_current->SetId(current->GetId() + 1);

    // swap current & retired
    replicas_[retired_idx].StoreRelease(new_current);
    current_idx_.StoreRelease(retired_idx);

    // new current & retired
    current = AcquireCurrent();
    retired = AcquireRetired();
    std::deque<std::int64_t> fail_idxs;

    auto promote_fn = [&](int64_t i) -> bool {
        Handle handle;
        bool has_handle = false;
        if (!retired->TryPickHandle(i, &handle, &has_handle)) {
            if (has_handle) {
                fail_idxs.push_back(i);
            }
            return false;
        }
        current->Insert(handle);
        return true;
    };

    for (int64_t i = 0; i < retired->GetLength(); ++i) {
        promote_fn(i);
    }
    while (fail_idxs.size() > 0) {
        std::deque<std::int64_t> idxs;
        idxs.swap(fail_idxs);
        for (int64_t i : idxs) {
            promote_fn(i);
        }
    }

    scaling_.StoreRelease(false);

    while (retired_ref_count_.LoadAcquire() > 0) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    delete retired;
    retired = nullptr;
    replicas_[1 - current_idx_.LoadAcquire()].StoreRelease(retired);
}

}  // namespace hyper_cache::clock
