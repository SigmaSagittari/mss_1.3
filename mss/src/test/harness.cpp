#include <chrono>
#include <iostream>

#include "test/common.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss::test;
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
}
