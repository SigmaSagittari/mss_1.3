#pragma once

#include <chrono>
#include <iomanip>

#include "test/common.h"

namespace mss::test {

struct RealGameProbabilityConfig {
    int rows;
    int cols;
    int mines;
    long long expectedPositions;
};

// 从真实对局收集指定数量的中间局面；只计 Exact::analyze 的耗时。
inline void testRealGameProbabilityPerformance(
    Rng& rng, const RealGameProbabilityConfig& config) {
    long long positions = 0;
    long long games = 0;
    long long unavailableMoves = 0;
    double seconds = 0.0;
    Distribution::DistPool distributions;
    GameConfig gameConfig;
    gameConfig.rows = config.rows;
    gameConfig.cols = config.cols;
    gameConfig.mines = config.mines;
    gameConfig.requireWinningGame = false;
    gameConfig.firstMoveSafe = true;

    while (positions < config.expectedPositions) {
        ++games;
        generateGame(gameConfig, rng, [&](const Snapshot& snapshot) {
            if (positions == config.expectedPositions) return;
            if (snapshot.next.x == 0) ++unavailableMoves;
            const auto start = std::chrono::steady_clock::now();
            const Probability::Result probability = Exact::analyze(snapshot.game.board,
                snapshot.analysis.basic, snapshot.analysis.structure, distributions);
            seconds += std::chrono::duration<double>(
                std::chrono::steady_clock::now() - start).count();
            ++positions;
            (void)probability;
        });
    }

    std::cout << "performance/real-game-probability: " << config.rows << 'x'
              << config.cols << ", " << std::fixed << std::setprecision(2)
              << static_cast<double>(config.mines) * 100.0 / (config.rows * config.cols)
              << "%: " << positions << " positions from " << games << " games, "
              << std::setprecision(3) << seconds << "s, " << positions / seconds
              << " positions/s, " << unavailableMoves << " unavailable moves\n";
}

}  // namespace mss::test
