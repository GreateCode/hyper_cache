#pragma once

#include <cstdint>

namespace cache::hash_skip {

static constexpr int64_t VERSION_MASK = 0x3FFFL; // 14 bits

class MarkVersionedPtr {
public:
  MarkVersionedPtr() noexcept : p_(0) {}
  MarkVersionedPtr(uintptr_t p) noexcept : p_(p) {}
  MarkVersionedPtr(void *p) noexcept : p_(reinterpret_cast<uintptr_t>(p)) {}

  bool operator==(const MarkVersionedPtr &m) const noexcept {
    return p_ == m.p_;
  }
  bool operator!=(const MarkVersionedPtr &m) const noexcept {
    return p_ != m.p_;
  }

  uintptr_t GetPtr() const noexcept { return p_; }
  void SetPtr(uintptr_t p) noexcept { p_ = p; }
  void *GetRawPtr() const noexcept {
    return reinterpret_cast<void *>(p_ & 0x00007FFFFFFFFFFF);
  }

  bool IsMarked() const noexcept { return p_ & (1L << 62); }
  MarkVersionedPtr Mark() noexcept { return p_ | 1L << 62; }
  MarkVersionedPtr UnMark() noexcept { return p_ & ~(1L << 62); }

  // int32_t GetVersion() const noexcept { return (p_ & (0x3FFFL << 48)) >> 48;
  // }
  int32_t GetVersion() const noexcept {
    return static_cast<int32_t>((p_ >> 48) & VERSION_MASK);
  }
  MarkVersionedPtr ClearVersion() const noexcept {
    return p_ & ~(0x3FFFL << 48);
  }
  MarkVersionedPtr SetVersion(int64_t version) const noexcept {
    return ClearVersion().p_ | ((version & VERSION_MASK) << 48);
  }
  MarkVersionedPtr IncrVersion() const noexcept {
    return SetVersion(GetVersion() + 1);
  }
  MarkVersionedPtr DecrVersion() const noexcept {
    return SetVersion(GetVersion() - 1);
  }

  static MarkVersionedPtr PackPtr(uintptr_t p, int64_t version, bool mark) {
    MarkVersionedPtr ptr(p);
    ptr = mark ? ptr.Mark() : ptr.UnMark();
    return ptr.SetVersion(version);
  }
  static MarkVersionedPtr PackPtr(void *p, int64_t version, bool mark) {
    return PackPtr(reinterpret_cast<uintptr_t>(p), version, mark);
  }

private:
  uintptr_t p_;
};

} // namespace cache::hash_skip
