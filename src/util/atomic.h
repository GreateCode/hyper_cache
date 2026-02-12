#pragma once

#include <atomic>

namespace hyper_cache {

template <typename T>
class RelaxedAtomic {
public:
    explicit RelaxedAtomic(T initial = {}) : v_(initial) {}
    void StoreRelaxed(T desired) { v_.store(desired, std::memory_order_relaxed); }
    T LoadRelaxed() const { return v_.load(std::memory_order_relaxed); }
    bool CasWeakRelaxed(T& expected, T desired) {
        return v_.compare_exchange_weak(expected, desired, std::memory_order_relaxed);
    }
    bool CasStrongRelaxed(T& expected, T desired) {
        return v_.compare_exchange_strong(expected, desired, std::memory_order_relaxed);
    }
    T ExchangeRelaxed(T desired) { return v_.exchange(desired, std::memory_order_relaxed); }
    T FetchAddRelaxed(T operand) { return v_.fetch_add(operand, std::memory_order_relaxed); }
    T FetchSubRelaxed(T operand) { return v_.fetch_sub(operand, std::memory_order_relaxed); }
    T FetchAndRelaxed(T operand) { return v_.fetch_and(operand, std::memory_order_relaxed); }
    T FetchOrRelaxed(T operand) { return v_.fetch_or(operand, std::memory_order_relaxed); }
    T FetchXorRelaxed(T operand) { return v_.fetch_xor(operand, std::memory_order_relaxed); }

protected:
    std::atomic<T> v_;
};

template <typename T>
class AcqRelAtomic : public RelaxedAtomic<T> {
public:
    explicit AcqRelAtomic(T initial = {}) : RelaxedAtomic<T>(initial) {}
    void StoreRelease(T desired) { RelaxedAtomic<T>::v_.store(desired, std::memory_order_release); }
    T LoadAcquire() const { return RelaxedAtomic<T>::v_.load(std::memory_order_acquire); }
    bool CasWeakRelease(T& expected, T desired) {
        return RelaxedAtomic<T>::v_.compare_exchange_weak(expected, desired, std::memory_order_release);
    }
    bool CasStrongRelease(T& expected, T desired) {
        return RelaxedAtomic<T>::v_.compare_exchange_strong(expected, desired, std::memory_order_release);
    }
    bool CasWeakAcqRel(T& expected, T desired) {
        return RelaxedAtomic<T>::v_.compare_exchange_weak(expected, desired, std::memory_order_acq_rel);
    }
    bool CasStrongAcqRel(T& expected, T desired) {
        return RelaxedAtomic<T>::v_.compare_exchange_strong(expected, desired, std::memory_order_acq_rel);
    }
    T ExchangeAcqRel(T desired) { return RelaxedAtomic<T>::v_.exchange(desired, std::memory_order_acq_rel); }
    T FetchAddAcqRel(T operand) { return RelaxedAtomic<T>::v_.fetch_add(operand, std::memory_order_acq_rel); }
    T FetchSubAcqRel(T operand) { return RelaxedAtomic<T>::v_.fetch_sub(operand, std::memory_order_acq_rel); }
    T FetchAndAcqRel(T operand) { return RelaxedAtomic<T>::v_.fetch_and(operand, std::memory_order_acq_rel); }
    T FetchOrAcqRel(T operand) { return RelaxedAtomic<T>::v_.fetch_or(operand, std::memory_order_acq_rel); }
    T FetchXorAcqRel(T operand) { return RelaxedAtomic<T>::v_.fetch_xor(operand, std::memory_order_acq_rel); }
};

}  // namespace hyper_cache
