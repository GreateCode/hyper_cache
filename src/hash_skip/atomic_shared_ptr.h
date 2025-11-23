#pragma once
#include <memory>

#include "hash_skip/ptr_util.h"
#include "util/atomic.h"

namespace cache::hash_skip {

#if 1
// 封装 std::shared_ptr<T> 的原子读写, support multi-load:multi-store
template <typename T> class AtomicSharedPtr {
public:
  AtomicSharedPtr() : ptr_(MarkVersionedPtr(nullptr)) {}

  explicit AtomicSharedPtr(std::shared_ptr<T> value) {
    std::shared_ptr<T> *raw_ptr =
        value ? new std::shared_ptr<T>(std::move(value)) : nullptr;
    ptr_.StoreRelaxed(MarkVersionedPtr::PackPtr(raw_ptr, 0, false));
  }

  ~AtomicSharedPtr() {
    auto *raw_ptr =
        static_cast<std::shared_ptr<T> *>(ptr_.LoadAcquire().GetRawPtr());
    if (raw_ptr) {
      delete raw_ptr;
      ptr_.StoreRelaxed(nullptr);
    }
  }

  // lockfree load, concurrency < max version(2**14)
  std::shared_ptr<T> Load() {
    MarkVersionedPtr curr_ptr = ptr_.LoadAcquire();
    // acquire ref
    do {
      if (!curr_ptr.GetRawPtr()) {
        return nullptr;
      }
      curr_ptr = curr_ptr.UnMark();
    } while (!ptr_.CasWeakAcqRel(curr_ptr, curr_ptr.IncrVersion()));

    std::shared_ptr<T> value;
    if (auto *raw_ptr = static_cast<std::shared_ptr<T> *>(curr_ptr.GetRawPtr());
        raw_ptr) {
      value = *raw_ptr;
    }

    // release ref
    do {
      curr_ptr = curr_ptr.UnMark();
    } while (!ptr_.CasWeakAcqRel(curr_ptr, curr_ptr.DecrVersion()));

    return value;
  }

#if 1
  std::shared_ptr<T> Store(std::shared_ptr<T> new_value) {
    std::shared_ptr<T> *new_raw_ptr =
        new_value ? new std::shared_ptr<T>(std::move(new_value)) : nullptr;

    MarkVersionedPtr old_ptr = ptr_.LoadAcquire();
    do {
      old_ptr = MarkVersionedPtr::PackPtr(old_ptr.GetRawPtr(), 0, false);
    } while (!ptr_.CasWeakAcqRel(old_ptr, old_ptr.Mark())); // mark

    //        while (true) {
    //            old_ptr = MarkVersionedPtr::PackPtr(old_ptr.GetRawPtr(), 0,
    //            false); if (ptr_.CasWeakAcqRel(old_ptr, old_ptr.Mark())) {
    //                break;
    //            }
    //            for (int i = 0; i < 10; ++i) {
    //                _mm_pause();
    //            }
    //        }

    MarkVersionedPtr new_ptr =
        MarkVersionedPtr::PackPtr(new_raw_ptr, 0, false); // unmark
    ptr_.StoreRelease(new_ptr);

    std::shared_ptr<T> old_value{nullptr};
    if (auto *old_raw_ptr =
            static_cast<std::shared_ptr<T> *>(old_ptr.GetRawPtr());
        old_raw_ptr) {
      old_value = *old_raw_ptr;
      delete old_raw_ptr;
    }
    return old_value;
  }
#endif

private:
  AcqRelAtomic<MarkVersionedPtr> ptr_;
};

#else

template <typename T> class AtomicSharedPtr {
public:
  AtomicSharedPtr() : ptr_(MarkVersionedPtr(nullptr)) {}

  explicit AtomicSharedPtr(std::shared_ptr<T> sp) {
    // ptr_.StoreRelaxed(sp);
    ptr_ = sp;
  }

  ~AtomicSharedPtr() {
    // ptr_.StoreRelaxed(nullptr);
  }

  // Lock-free 获取
  std::shared_ptr<T> Load() {
    // return std::atomic_load_explicit(&ptr_, std::memory_order_acquire);
    return std::atomic_load(&ptr_, std::memory_order_acquire);
  }

  // 原子更新，返回旧的值
  std::shared_ptr<T> Store(std::shared_ptr<T> sp) {
    // return std::atomic_exchange_explicit(&ptr_, sp,
    // std::memory_order_acq_rel);
    return std::atomic_exchange(&ptr_, sp, std::memory_order_acq_rel);
  }

private:
  std::atomic<std::shared_ptr<T>> ptr_;
};
#endif

} // namespace cache::hash_skip
