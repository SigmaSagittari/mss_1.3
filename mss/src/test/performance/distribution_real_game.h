#pragma once

#include <chrono>
#include <iostream>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution/distribution.h"
#include "analysis/distribution/distribution_graph.h"
#include "analysis/structure.h"
#include "test/common.h"

namespace mss::test {

// 真实对局中的分布层性能测试（统一时间盒驱动）：揭示推进走 Basic::update +
// Structure::update（buildComponent 所在的增量路径），每个局面遍历全部组件
// 跑分布分析。config.distributionMode 决定用旧 Solver 还是图算法实现。
inline void testDistributionRealGame(Rng& rng, const TestConfig& config) {
    const bool useOld = config.distributionMode == DistributionMode::Old;

    TimeBox timebox(config.seconds);
    long long positions = 0;
    long long games = 0;
    long long components = 0;
    long long entries = 0;
    double analyzeSeconds = 0.0;
    struct ClassStats {
        long long components = 0;
        long long entries = 0;
        double analyzeSeconds = 0.0;
    };
    std::vector<ClassStats> classes;
    while ((config.games < 0 || games < config.games) && !timebox.expired()) {
        ++games;
        Game game(config);
        game.placeMines(rng, config.firstMoveSafe);
        Basic::Delta firstMove;
        game.reveal(1, 1, firstMove);
        Basic::Result basic = Basic::analyze(game.board);
        Structure::ShapePool shapes;
        Structure::Result structure = Structure::analyze(game.board, basic, shapes);
        Distribution::DistPool oldPool;
        DistributionSolver::DistPool graphPool;

        while (!game.won() && !timebox.expired()) {
            for (const Structure::Instance& instance : structure.components) {
                const std::chrono::steady_clock::time_point analyzeStart =
                    std::chrono::steady_clock::now();
                long long entryCount = 0;
                if (useOld) {
                    const Distribution* old =
                        Distribution::Solver::analyze(*instance.shape, oldPool);
                    entryCount = static_cast<long long>(old->entries.size());
                } else {
                    const DistributionSolver::Distribution* graph =
                        DistributionSolver::analyze(*instance.shape, graphPool);
                    entryCount = static_cast<long long>(graph->entries.size());
                }
                const double analyzeElapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - analyzeStart).count();
                analyzeSeconds += analyzeElapsed;

                const int boxCount = static_cast<int>(instance.boxes.count());
                if (static_cast<int>(classes.size()) <= boxCount)
                    classes.resize(boxCount + 1);
                ClassStats& stats = classes[boxCount];
                stats.components++;
                stats.entries += entryCount;
                stats.analyzeSeconds += analyzeElapsed;
                entries += entryCount;
                ++components;
            }
            ++positions;

            int nextX = 0;
            int nextY = 0;
            for (int x = 1; x <= game.board.rows && nextX == 0; ++x)
                for (int y = 1; y <= game.board.cols; ++y)
                    if (game.board.board[x][y] == Cell::Hidden && !game.mine(x, y)) {
                        nextX = x;
                        nextY = y;
                        break;
                    }

            Basic::Delta updates;
            game.reveal(nextX, nextY, updates);
            Basic::update(game.board, basic, updates);
            Structure::update(game.board, basic, structure, shapes, updates);
        }
    }
    const double seconds = timebox.elapsedSeconds();
    std::cout << "performance/distribution-real-game: " << positions << " positions from "
              << games << " games, " << components << " components, " << seconds << "s\n"
              << "  " << (useOld ? "old" : "graph") << "=" << analyzeSeconds
              << "s, entries=" << entries << '\n';
    for (int boxCount = 0; boxCount < static_cast<int>(classes.size()); ++boxCount) {
        const ClassStats& stats = classes[boxCount];
        if (stats.components == 0) continue;
        std::cout << "  boxes=" << boxCount << ": components=" << stats.components
                  << ", " << (useOld ? "old" : "graph") << "=" << stats.analyzeSeconds
                  << "s, entries=" << stats.entries << '\n';
    }
}

}  // namespace mss::test