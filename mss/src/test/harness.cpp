#include "test/common.h"
#include "test/basic/flood_probability.h"
#include "test/distribution/greedy_order.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss::test;
    constexpr TestConfig normal_test{
        .rows = 30, .cols = 30, .mines = 225, .expectedPositions = 10000000,
        .firstMoveSafe = true,
    };

    Rng rng(0xC0FFEE12345ULL);
    //testProbabilitySpread(rng, normal_test);
    //testBasicFloodProbability(rng, normal_test);
    testDistributionOrders(rng, normal_test);
    //testRealGameProbabilityPerformance(rng, normal_test);
    return 0;
}
