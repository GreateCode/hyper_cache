#include <clock/shard.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iostream>
#include <thread>

#include "clock/clock_cache.h"
#include "clock/handle.h"

namespace hyper_cache::clock {

size_t RoundUpToPowerOf2(int32_t x) {
    size_t n = 1;
    while (n < x) {
        n <<= 1;
    }
    return n;
}

ShardWrapper::ShardWrapper(ClockCacheOptions options) {
    ClockCache* cc = new ClockCache(options);
    cc->SetId(0);

    replicas_[0].StoreRelease(cc);

    current_idx_.StoreRelease(0);
}

ShardWrapper::~ShardWrapper() {
    for (int32_t i = 0; i < replicas_.size(); ++i) {
        ClockCache* cc = replicas_[i].LoadAcquire();
        if (!cc) {
            continue;
        }
        assert(cc->PinCount() == 0);
        delete cc;
        replicas_[i].StoreRelease(nullptr);
    }
}

bool ShardWrapper::Lookup(const UniqueId64x2& hashed_key, HandlePin* handle_pin) {
    ClockCache* current = AcquireCurrent();
    if (current) {
        // protect slow Lookup: replica may become retired before return
        return current->Lookup(hashed_key, handle_pin);
    }

    if (!scaling_.LoadAcquire()) {
        return false;
    }

    ClockCache* retired = AcquireRetired();
    if (!retired) {
        return false;
    }
    return retired->Lookup(hashed_key, handle_pin);
}

HandleImpl* ShardWrapper::Lookup(const UniqueId64x2& hashed_key) {
    ClockCache* current = AcquireCurrent();
    if (current) {
        return current->Lookup(hashed_key);
    }
    return nullptr;
}

bool ShardWrapper::Insert(const Handle& handle) {
    ClockCache* current = AcquireCurrent();
    if (!current) {
        return false;
    }
    return current->Insert(handle);
}

bool ShardWrapper::Release(HandlePin* handle_pin) {
    if (!handle_pin || !handle_pin->handle) {
        return false;
    }
    uint8_t id = handle_pin->handle->Id();
    for (int i = 0; i < replicas_.size(); ++i) {
        ClockCache* replica = replicas_[i].LoadAcquire();
        if (!replica) {
            continue;
        }
        if (id == replica->GetId()) {
            replica->Release(handle_pin);
            return true;
        }
    }
    return false;
}

bool ShardWrapper::Release(HandleImpl* handle) {
    ClockCache* current = AcquireCurrent();
    if (!current) {
        return false;
    }
    return current->Release(handle);
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

    ClockCache* retired = AcquireRetired();
    if (!retired) {
        return succ;
    }
    succ &= retired->Erase(hashed_key);
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

bool ShardWrapper::SmoothScale() {
    ClockCache* current = AcquireCurrent();
    if (!current) {
        return false;
    }
    ClockCacheOptions options = current->Options();
    if (current->GetOccupancy() * 1.0 / current->GetLength() < options.load_factor * 0.9 ||
        current->GetUsage() > options.capacity * 0.9) {
        return false;  // no need to scale
    }

    bool expect = false;
    if (!scaling_.CasStrongAcqRel(expect, true)) {
        return false;
    }

    // std::cout << "SmoothScale start.." << std::endl;

    const int32_t retired_idx = 1 - current_idx_.LoadAcquire();
    ClockCache* retired       = replicas_[retired_idx].LoadAcquire();
    if (retired) {
        delete retired;
        retired = nullptr;
        replicas_[retired_idx].StoreRelease(nullptr);
    }

    const double avg_usage  = current->GetUsage() * 1.0 / current->GetOccupancy();
    const int32_t length    = options.capacity / (avg_usage * options.load_factor);
    options.length          = RoundUpToPowerOf2(length);
    ClockCache* new_current = new ClockCache(options);
    new_current->SetId(current->GetId() + 1);

    // swap current & retired
    replicas_[retired_idx].StoreRelease(new_current);
    current_idx_.StoreRelease(retired_idx);

    // new current & retired
    current = AcquireCurrent();
    retired = AcquireRetired();
    retired->SetRetired(true);

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

    while (true) {
        const int32_t fail_count = fail_idxs.size();
        if (fail_count > 0 && fail_count < 1000) {
            scaling_.StoreRelease(false);
        }
        if (fail_count == 0) {
            if (scaling_.LoadAcquire()) {
                scaling_.StoreRelease(false);
            }
            break;
        }
        std::deque<std::int64_t> idxs;
        idxs.swap(fail_idxs);
        for (int64_t i : idxs) {
            promote_fn(i);
        }
        // Backoff so worker threads can Release(); avoid busy-spin that blocks progress.
        if (!fail_idxs.empty()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }

    while (retired->PinCount() > 0) {
        std::cout << "wait for pin count to be 0, current pin count: " << retired->PinCount() << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    delete retired;
    retired = nullptr;
    replicas_[1 - current_idx_.LoadAcquire()].StoreRelease(retired);

    // std::cout << "SmoothScale end" << std::endl;
    return true;
}

}  // namespace hyper_cache::clock
