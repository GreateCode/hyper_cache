#include "util/util.h"

#include <cstdint>
#include <ctime>

namespace hyper_cache {

double FastRandomDouble() {
    struct XorShiftState {
        uint64_t s[2];
        XorShiftState() {
            uint64_t seed = (uint64_t)this + (uint64_t)time(nullptr);
            s[0]          = seed;
            s[1]          = seed * 0x5deece66d + 0xb;
        }
    };

    static thread_local XorShiftState state;

    uint64_t x       = state.s[0];
    uint64_t const y = state.s[1];
    state.s[0]       = y;
    x ^= x << 23;
    state.s[1] = x ^ y ^ (x >> 17) ^ (y >> 26);

    uint64_t rand_int       = state.s[1] + y;
    constexpr uint64_t mask = (1ULL << 52) - 1;
    return ((1.0) + ((rand_int & mask) * (1.0 / (1ULL << 52)))) - 1.0;
}

}  // namespace hyper_cache