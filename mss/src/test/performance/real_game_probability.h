#pragma once

#include <iomanip>

#include "test/common.h"

namespace mss::test {

// 真实对局的全流程吞吐（统一时间盒驱动）：不再单独统计 Exact::analyze 的
// 耗时——整条路径（对局生成 + lowestRiskMove + basic/structure 增量分析 +
// 精确概率）在时间盒内能完成多少局面，就是当前实现的端到端吞吐（与各 perf
// 模块同规格，改动可用同种子对跑直接对比）。
inline void testRealGameProbabilityPerformance(const unsigned long long& seed, const TestConfig& config) {
    Rng rng(seed);
    long long positions = 0;
    long long unavailableMoves = 0;
    const RunSummary summary = runGames(config, rng, [&](const Snapshot& snapshot) {
        if (snapshot.next.x == 0) ++unavailableMoves;
        ++positions;
    });

    const double seconds = summary.elapsedSeconds;
    std::cout << "performance/real-game-probability: " << config.rows << 'x'
              << config.cols << ", " << std::fixed << std::setprecision(2)
              << static_cast<double>(config.mines) * 100.0 / (config.rows * config.cols)
              << "%: " << positions << " positions from " << summary.games << " games, "
              << std::setprecision(3) << seconds << "s, " << positions / seconds
              << " positions/s, " << unavailableMoves << " unavailable moves\n";
}

}  // namespace mss::test