#pragma once

#include <array>
#include <chrono>
#include <iomanip>

#include "test/common.h"

namespace mss::test {

// 真实对局产生的中间局面：只计 Exact::analyze 的耗时，不做正确性断言。
inline void testRealGameProbabilityPerformance(Rng& rng, long long targetPositions) {
    struct Config {
        int rows;
        int cols;
        int mines;
    };
    constexpr std::array configs{
        Config{100, 100, 2183},
        Config{100, 100, 2400},
        Config{100, 100, 2485},
        Config{200, 200, 8000},
        Config{30, 30, 225},
    };

    for (const Config config : configs) {
        long long positions = 0;
        long long games = 0;
        long long unavailableMoves = 0;
        long double firstUnavailableCandidates = 0.0L;
        long double firstUnavailableUnknownProbability = 0.0L;
        double seconds = 0.0;
        Distribution::DistPool distributions;
        GameConfig gameConfig;
        gameConfig.rows = config.rows;
        gameConfig.cols = config.cols;
        gameConfig.mines = config.mines;
        gameConfig.requireWinningGame = false;
        gameConfig.firstMoveSafe = true;

        while (positions < targetPositions) {
            ++games;
            generateGame(gameConfig, rng, [&](const Snapshot& snapshot) {
                if (snapshot.next.x == 0) {
                    if (unavailableMoves == 0) {
                        firstUnavailableCandidates = snapshot.analysis.probability.candidates;
                        firstUnavailableUnknownProbability =
                            snapshot.analysis.probability.tCellProbability;
                    }
                    ++unavailableMoves;
                }
                const auto start = std::chrono::steady_clock::now();
                const Probability::Result probability = Exact::analyze(snapshot.game.board,
                    snapshot.analysis.basic, snapshot.analysis.structure, distributions);
                seconds += std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count();
                ++positions;
                (void)probability;
            });
        }

        const double positionsPerSecond = positions / seconds;
        std::cout << "performance/real-game-probability: " << config.rows << 'x'
                  << config.cols << ", " << std::fixed << std::setprecision(2)
                  << static_cast<double>(config.mines) * 100.0 / (config.rows * config.cols)
                  << "%: " << positions << " positions from " << games << " games, "
                  << std::setprecision(3) << seconds << "s, "
                  << positionsPerSecond << " positions/s, " << unavailableMoves
                  << " unavailable moves, first candidates="
                  << firstUnavailableCandidates << ", first unknown p="
                  << firstUnavailableUnknownProbability << '\n';
    }
}

}  // namespace mss::test
