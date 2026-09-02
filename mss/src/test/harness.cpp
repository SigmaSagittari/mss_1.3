#include <chrono>

#include "test/common.h"
#include "test/basic/flood_probability.h"
#include "test/distribution/greedy_order.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss;
    using namespace mss::test;
    constexpr TestConfig normal_test{
        .rows = 30, .cols = 30, .mines = 225,
        .firstMoveSafe = true,
    };

    mss::test::Rng rng(0xC0FFEE12345ULL);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(20);
    long long positions = 0;
    long long games = 0;
    long long components = 0;
    long long entries = 0;
    double oldSeconds = 0.0;
    double graphSeconds = 0.0;
    struct ClassStats {
        long long components = 0;
        long long entries = 0;
        double oldSeconds = 0.0;
        double graphSeconds = 0.0;
    };
    std::vector<ClassStats> classes;
    while (std::chrono::steady_clock::now() < deadline) {
        ++games;
        Game game(normal_test);
        game.placeMines(rng, normal_test.firstMoveSafe);
        Basic::Delta firstMove;
        game.reveal(1, 1, firstMove);
        Basic::Result basic = Basic::analyze(game.board);
        Structure::ShapePool shapes;
        Structure::Result structure = Structure::analyze(game.board, basic, shapes);
        Distribution::DistPool oldPool;
        DistributionSolver::DistPool graphPool;

        while (!game.won() && std::chrono::steady_clock::now() < deadline) {
            for (const Structure::Instance& instance : structure.components) {
                const auto oldStart = std::chrono::steady_clock::now();
                const Distribution* old = Distribution::Solver::analyze(*instance.shape, oldPool);
                const double oldElapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - oldStart).count();
                oldSeconds += oldElapsed;

                const auto graphStart = std::chrono::steady_clock::now();
                const DistributionSolver::Distribution* graph =
                    DistributionSolver::analyze(*instance.shape, graphPool);
                const double graphElapsed = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - graphStart).count();
                graphSeconds += graphElapsed;

                int nodeClass = 0;
                for (std::uint64_t nodes = old->searchNodes; nodes >= 2; nodes >>= 1)
                    ++nodeClass;
                if (static_cast<int>(classes.size()) <= nodeClass)
                    classes.resize(nodeClass + 1);
                ClassStats& stats = classes[nodeClass];
                const long long entryCount =
                    static_cast<long long>(old->entries.size() + graph->entries.size());
                stats.components++;
                stats.entries += entryCount;
                stats.oldSeconds += oldElapsed;
                stats.graphSeconds += graphElapsed;
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
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "performance/distribution-real-game: " << positions << " positions from "
              << games << " games, " << components << " components, " << seconds << "s\n"
              << "  old=" << oldSeconds << "s, graph=" << graphSeconds << "s, entries="
              << entries << '\n';
    for (int nodeClass = 0; nodeClass < static_cast<int>(classes.size()); ++nodeClass) {
        const ClassStats& stats = classes[nodeClass];
        if (stats.components == 0) continue;
        const std::uint64_t lower = std::uint64_t{1} << nodeClass;
        const std::uint64_t upper = lower << 1;
        std::cout << "  nodes=[" << lower << ',' << upper << "): components="
                  << stats.components << ", old=" << stats.oldSeconds << "s, graph="
                  << stats.graphSeconds << "s, entries=" << stats.entries << '\n';
    }
    return 0;
}
