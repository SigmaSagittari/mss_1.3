#pragma once

#include <cstdint>

namespace mss {

// SplitMix64 伪随机数生成器。
inline std::uint64_t splitmix64(std::uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

struct Rng {
    std::uint64_t state = 0;

    explicit Rng(std::uint64_t seed = 0) : state(seed) {}

    std::uint64_t next() {
        state = splitmix64(state);
        return state;
    }

    // 返回 [0, 1) 内的均匀随机数。
    long double nextUnit() {
        return static_cast<long double>(next() & 0xFFFFFFFFFFFFFULL) /
               static_cast<long double>(1ULL << 52);
    }
};

}  // namespace mss
