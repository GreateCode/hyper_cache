#pragma once

#include <array>
#include <cstddef>

#include "util/atomic.h"

namespace hyper_cache::clock {

using ObjectPtr = void *;
using DeleterFn = void (*)(ObjectPtr obj);
using UniqueId64x2 = std::array<uint64_t, 2>;

struct Handle {
  ObjectPtr value = nullptr;
  DeleterFn del_cb = nullptr;
  UniqueId64x2 hashed_key = {0, 0};
  size_t total_charge = 0;

  size_t GetTotalCharge() const { return total_charge; }

  void FreeData() {
    if (del_cb && value) {
      del_cb(value);
      value = nullptr;
    }
  }

  const UniqueId64x2 &GetHash() const { return hashed_key; }
};

struct ClockHandle : public Handle {
  // Constants for handling the atomic `meta` word, which tracks most of the
  // state of the handle. The meta word looks like this:
  // low bits                                                     high bits
  // -----------------------------------------------------------------------
  // | refing counter       | hit counter         | hit bit | state marker |
  // -----------------------------------------------------------------------

  // For reading or updating counters in meta word.
  static constexpr uint8_t kCounterNumBits = 30;
  static constexpr uint64_t kCounterMask = (uint64_t{1} << kCounterNumBits) - 1;

  static constexpr uint8_t kRefCounterShift = 0;
  static constexpr uint64_t kRefIncrement = uint64_t{1} << kRefCounterShift;
  static constexpr uint8_t kHitCounterShift = kCounterNumBits;
  static constexpr uint64_t kHitIncrement = uint64_t{1} << kHitCounterShift;

  static constexpr uint8_t kHitBitShift = 2U * kCounterNumBits;
  static constexpr uint64_t kHitBitMask = uint64_t{1} << kHitBitShift;

  static constexpr uint8_t kStateShift = kHitBitShift + 1;

  static constexpr uint8_t kStateOccupiedBit = 0b100;
  static constexpr uint8_t kStateShareableBit = 0b010;
  static constexpr uint8_t kStateVisibleBit = 0b001;

  static constexpr uint8_t kStateEmpty = 0b000;
  static constexpr uint8_t kStateConstruction = kStateOccupiedBit;
  static constexpr uint8_t kStateInvisible =
      kStateOccupiedBit | kStateShareableBit;
  static constexpr uint8_t kStateVisible =
      kStateOccupiedBit | kStateShareableBit | kStateVisibleBit;

  static constexpr uint8_t kHighCountdown = 3;
  static constexpr uint8_t kLowCountdown = 2;
  static constexpr uint8_t kBottomCountdown = 1;
  static constexpr uint8_t kMaxCountdown = kHighCountdown;

  mutable AcqRelAtomic<uint64_t> meta{};
}; // struct ClockHandle

struct alignas(64U) HandleImpl : public ClockHandle {
  RelaxedAtomic<uint32_t> displacements{};
};

} // namespace hyper_cache::clock