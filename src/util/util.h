#pragma once

#include <random>

#ifndef likely
#define likely(x) __builtin_expect(!!(x), 1)
#define unlikely(x) __builtin_expect(!!(x), 0)
#endif

namespace cache {

double FastRandomDouble();

class ThreadSafeNormalRandom {
public:
  ThreadSafeNormalRandom(double mean = 0.0, double stddev = 10.0)
      : mean_(mean), stddev_(stddev) {}

  double operator()() {
    thread_local std::mt19937 gen{std::random_device{}()};
    thread_local std::normal_distribution<double> dist(mean_, stddev_);
    return dist(gen);
  }

private:
  double mean_;
  double stddev_;
};

} // namespace cache