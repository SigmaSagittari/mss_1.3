#include "test/common.h"
#include "test/performance/real_game_probability.h"

int main() {
    using namespace mss::test;
    constexpr RealGameProbabilityConfig perf100x100_2183{100, 100, 2183, 70000};
    constexpr RealGameProbabilityConfig perf100x100_2400{100, 100, 2400, 30000};
    constexpr RealGameProbabilityConfig perf100x100_2485{100, 100, 2485, 10000};
    constexpr RealGameProbabilityConfig perf30x30_2500{30, 30, 225, 130000};

    Rng rng(0xC0FFEE12345ULL);
    //testRealGameProbabilityPerformance(rng, perf100x100_2183);
    //testRealGameProbabilityPerformance(rng, perf100x100_2400);
    //testRealGameProbabilityPerformance(rng, perf100x100_2485);
    testRealGameProbabilityPerformance(rng, perf30x30_2500);
    return 0;
}
