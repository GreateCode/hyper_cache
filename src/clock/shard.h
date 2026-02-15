#pragma once

#include <clock/clock_cache.h>
#include <util/atomic.h>

#include <array>
#include <cstdint>
#include <thread>

namespace hyper_cache::clock {

class ShardWrapper {
public:
    ShardWrapper(ClockCacheOptions options);
    ~ShardWrapper();

    HandleImpl* Lookup(const UniqueId64x2& hashed_key);
    bool Insert(const Handle& handle);
    bool Release(HandleImpl* handle);
    bool Erase(const UniqueId64x2& hashed_key);

    const HandleImpl* HandlePtr(size_t idx) const;

    size_t GetCapacity() const;

    size_t GetOccupancy() const;

    size_t GetUsage() const;

    size_t GetLength() const;

    size_t GetOccupancyLimit() const;

private:
    ClockCache* AcquireCurrent() const;
    ClockCache* AcquireRetired() const;

    void SmoothScale();

private:
    std::array<AcqRelAtomic<ClockCache*>, 2> replicas_;
    AcqRelAtomic<int32_t> current_idx_{0};
    AcqRelAtomic<int32_t> retired_ref_count_{0};
    AcqRelAtomic<bool> scaling_{false};
    std::thread scale_thread_;
};

}  // namespace hyper_cache::clock
