// test/harness.cpp — 全模块驱动。架构规范见 test/common.h 头部（改 test
// 代码之前必读）。main 只做三件事：显式写出各模块的 TestConfig（无默认值）、
// 用注释开关选择启用哪些模块、汇总 counters() 得到退出码。各模块输出自身
// 的统计行；驱动循环一律走 common.h 的 runGames。
#include <iostream>

#include "test/common.h"
#include "test/distribution/greedy_order.h"
#include "test/move_hash.h"
#include "test/performance/distribution_real_game.h"
#include "test/performance/real_game_probability.h"
#include "test/probability/conservation.h"
#include "test/structure/update_workspace.h"

int main() {
    using namespace mss;
    using namespace mss::test;

    // TestConfig 无默认值（规范 R3）：全部字段必须显式写在这里，方便手动改。
    // 驱动量二选一：seconds / games 其中一个为 -1 表示由另一个控制；
    // 双 -1 是配置错误，由 runGames 拒绝。
    TestConfig normal_test{
        .rows = 30,
        .cols = 30,
        .mines = 225,
        .seconds = 20,        // -1 = 不用时间盒（由 games 控制）
        .games = -1,          // 20000 局
        .filter = PositionFilter::All,
        .distributionMode = DistributionMode::Old,
        .maxRestarts = 10000,
        .requireWinningGame = false,
        .firstMoveSafe = true,
    };

    // 启用矩阵（规范 R2）：注释开关，一行一模块。默认只开性能面板。
    // seed 固定、永久不变（规范 R1）：所有模块从同一 seed 独立起流。
    const unsigned long long seed = 0xC0FFEE12345ULL;

    //testProbabilityConservation(seed, normal_test);
    //testDistributionOrders(seed, normal_test);
    //testDistributionRealGame(seed, normal_test);
    testRealGameProbabilityPerformance(seed, normal_test);
    testMoveHashes(seed, normal_test); // 基线：move - hash - total: b153b2b60f75dd7c(20000 games, 3198953 moves)
    //testUpdateWorkspaceClean(seed, normal_test);

    const long long failures = counters().failures;
    if (failures == 0) {
        std::cout << "harness: all checks passed\n";
    } else {
        std::cout << "harness: " << failures << " failures\n";
    }
    return failures == 0 ? 0 : 1;
}