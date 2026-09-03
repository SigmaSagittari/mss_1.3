// test/harness.cpp — 全模块驱动：统一配置（时间盒 seconds 或局数 games
// 二选一驱动，TestConfig 无默认值、全部字段在此显式写出）与统一接口
// （inline void testXxx(Rng&, const TestConfig&)），逐模块顺序执行。
// 各模块输出自身的吞吐/统计行；main 只负责汇总失败并给出退出码。
#include <iostream>

#include "test/common.h"
#include "test/basic/flood_probability.h"
#include "test/distribution/greedy_order.h"
#include "test/move_hash.h"
#include "test/performance/distribution_real_game.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss;
    using namespace mss::test;

    // TestConfig 无默认值：全部字段必须显式写在这里，方便手动改。
    // 驱动量二选一：seconds / games 其中一个为 -1 表示由另一个控制；
    // 两个都是 -1 是配置错误（下方 logerr 退出）。
    TestConfig normal_test{
        .rows = 30,
        .cols = 30,
        .mines = 225,
        .seconds = -1.0,        // -1 = 不用时间盒（由 games 控制）
        .games = 20000,          // 2000 局
        .filter = PositionFilter::All,
        .distributionMode = DistributionMode::Old,
        .maxRestarts = 10000,
        .requireWinningGame = false,
        .firstMoveSafe = true,
    };

    if (normal_test.seconds == -1.0 && normal_test.games == -1) {
        logerr("TestConfig: seconds 与 games 同为 -1，必须二选一指定驱动量");
        return 1;
    }

    mss::test::Rng rng(0xC0FFEE12345ULL);  // 全限定：与 core 的 mss::Rng 区分

    //testBasicFloodProbability(rng, normal_test);
    //testProbabilitySpread(rng, normal_test);
    //testDistributionOrders(rng, normal_test);
    //testDistributionRealGame(rng, normal_test);
    testRealGameProbabilityPerformance(rng, normal_test);
    //testMoveHashes(rng, normal_test);

    const long long failures = counters().failures;
    if (failures == 0) {
        std::cout << "harness: all checks passed\n";
    } else {
        std::cout << "harness: " << failures << " failures\n";
    }
    return failures == 0 ? 0 : 1;
}