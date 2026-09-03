// test/harness.cpp — 全模块驱动：统一配置（含统一时间盒 seconds）与统一
// 接口（inline void testXxx(Rng&, const TestConfig&)），逐模块顺序执行。
// 各模块输出自身的吞吐/统计行；main 只负责汇总失败并给出退出码。
#include <iostream>

#include "test/common.h"
#include "test/basic/flood_probability.h"
#include "test/distribution/greedy_order.h"
#include "test/performance/distribution_real_game.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss;
    using namespace mss::test;

    TestConfig normal_test{
        .rows = 30, .cols = 30, .mines = 225,
        .distributionMode = DistributionMode::Graph,
		.firstMoveSafe = true,
    };

    mss::test::Rng rng(0xC0FFEE12345ULL);  // 全限定：与 core 的 mss::Rng 区分

    //testBasicFloodProbability(rng, normal_test);
    //testProbabilitySpread(rng, normal_test);
    //testDistributionOrders(rng, normal_test);
    //testDistributionRealGame(rng, normal_test);
    testRealGameProbabilityPerformance(rng, normal_test);

    const long long failures = counters().failures;
    if (failures == 0) {
        std::cout << "harness: all checks passed\n";
    } else {
        std::cout << "harness: " << failures << " failures\n";
    }
    return failures == 0 ? 0 : 1;
}