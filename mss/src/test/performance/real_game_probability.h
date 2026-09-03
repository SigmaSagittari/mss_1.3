#pragma once

#include <iomanip>

#include "test/common.h"

namespace mss::test {

// 真实对局的全流程吞吐（统一时间盒驱动）：不再单独统计 Exact::analyze 的
// 耗时——整条路径（对局生成 + lowestRiskMove + basic/structure 增量分析 +
// 精确概率）在时间盒内能完成多少局面，就是当前实现的端到端吞吐（与各 perf
// 模块同规格，改动可用同种子对跑直接对比）。
inline void testRealGameProbabilityPerformance(Rng& rng, const TestConfig& config) {
    TimeBox timebox(config.seconds);
    long long positions = 0;
    long long games = 0;
    long long unavailableMoves = 0;
    while (!timebox.expired()) {
        ++games;
        generateGame(config, rng, [&](const Snapshot& snapshot) {
            if (timebox.expired()) return;
            if (snapshot.next.x == 0) ++unavailableMoves;
            ++positions;
        });
    }

    const double seconds = timebox.elapsedSeconds();
    std::cout << "performance/real-game-probability: " << config.rows << 'x'
              << config.cols << ", " << std::fixed << std::setprecision(2)
              << static_cast<double>(config.mines) * 100.0 / (config.rows * config.cols)
              << "%: " << positions << " positions from " << games << " games, "
              << std::setprecision(3) << seconds << "s, " << positions / seconds
              << " positions/s, " << unavailableMoves << " unavailable moves\n";
}

}  // namespace mss::test