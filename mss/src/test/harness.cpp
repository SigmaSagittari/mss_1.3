#include <chrono>
#include <iostream>

#include "test/common.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss::test;
#if defined(MSS_TEST_PERFORMANCE)
    Rng rng(0xC0FFEE12345ULL);
    // 宏值是每个配置需收集的有效概率局面数，而非对局数。
    testRealGameProbabilityPerformance(rng, MSS_TEST_PERFORMANCE);
    return 0;
#else
#if defined(MSS_TEST_ITER)
    constexpr int iter = MSS_TEST_ITER;
#else
    constexpr int iter = 1000;
#endif
    Rng rng(0xC0FFEE12345ULL);
    const auto start = std::chrono::steady_clock::now();
    testProbabilitySpread(rng, iter);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::cout << "spread/probability: " << iter << " games, " << counters().checks
              << " checks, " << counters().failures << " failures, " << seconds << "s\n";
    printProbabilityExtremes();
    return counters().failures == 0 ? 0 : 1;
#endif
}
