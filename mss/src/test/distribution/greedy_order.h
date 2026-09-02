#pragma once

#include <algorithm>
#include <iostream>
#include <vector>

#include "analysis/distribution/distribution_graph.h"
#include "test/common.h"

namespace mss::test {

// 友元测试探针：distribution_graph.h 的 Graph 与展开序产线已全部私有
// （对应其中的 mss::test::OrderProbe 前置声明与友元）。组装图、合法性
// 校验与两种抛光基准成品都只能经它完成。
struct OrderProbe {
    static DistributionSolver::Graph makeGreedyOrderGraph(const Structure::Shape& shape) {
        return DistributionSolver::Graph::fromShape(shape);
    }

    static int boxCount(const DistributionSolver::Graph& graph) {
        return graph.boxCount();
    }

    static void printOrderWidthGraph(const DistributionSolver::Graph& graph) {
        for (BoxId box = 0; box < graph.boxCount(); ++box)
            for (BoxId neighbor : graph.neighbors(box))
                if (box < neighbor) std::cout << box << ' ' << neighbor << '\n';
    }

    // 合法展开顺序：首项为 init；后续每个 box 必须只出现一次，且连接到已经
    // 选择的部分。顺便在线维护并返回该顺序的最大 state width。
    static bool checkOrder(const DistributionSolver::Graph& graph, BoxId init,
                           const std::vector<BoxId>& order, int& maxWidth) {
        const int n = graph.boxCount();
        if (static_cast<int>(order.size()) != n || order[0] != init) return false;

        std::vector<char> selected(n, 0);
        std::vector<int> unselectedNeighbors(n, 0);
        for (BoxId box = 0; box < n; ++box)
            unselectedNeighbors[box] = static_cast<int>(graph.neighbors(box).size());

        int width = 0;
        maxWidth = 0;
        for (int step = 0; step < n; ++step) {
            const BoxId box = order[step];
            if (box < 0 || box >= n || selected[box]) return false;
            if (step != 0) {
                bool touchesSelected = false;
                for (BoxId neighbor : graph.neighbors(box))
                    if (selected[neighbor]) {
                        touchesSelected = true;
                        break;
                    }
                if (!touchesSelected) return false;
            }

            for (BoxId neighbor : graph.neighbors(box)) {
                --unselectedNeighbors[neighbor];
                if (selected[neighbor] && unselectedNeighbors[neighbor] == 0) --width;
            }
            selected[box] = 1;
            if (unselectedNeighbors[box] != 0) ++width;
            maxWidth = std::max(maxWidth, width);
        }
        return true;
    }

    static std::vector<BoxId> lexBfsPolished(const DistributionSolver::Graph& graph) {
        std::vector<BoxId> order;
        graph.polishAdjacent(graph.diameterStart(), order);
        return order;
    }
    static std::vector<BoxId> lexBfsWindow3(const DistributionSolver::Graph& graph) {
        std::vector<BoxId> order;
        graph.polishWindow3(graph.diameterStart(), order);
        return order;
    }
    // 展开起点（= 旧 findDiameter().first，行为不变）。
    static BoxId start(const DistributionSolver::Graph& graph) {
        return graph.diameterStart();
    }
};

inline void printOrderWidthBoard(const ObservedBoard& board) {
    std::cout << board.rows << 'x' << board.cols << '/' << board.totalMines << '\n';
    for (int x = 1; x <= board.rows; ++x) {
        for (int y = 1; y <= board.cols; ++y) {
            const Cell cell = board.board[x][y];
            std::cout << (cell == Cell::Hidden ? 'H'
                                                : static_cast<char>('0' + numberValue(cell)));
        }
        std::cout << '\n';
    }
}

struct OrderStats {
    long long legal = 0;
    long long totalMaxWidth = 0;
    int greatestWidth = 0;
    long double totalTwos = 0.0L;
    long double totalWays = 0.0L;
    std::vector<long long> widthDistribution;

    void note(int maxWidth) {
        ++legal;
        totalMaxWidth += maxWidth;
        greatestWidth = std::max(greatestWidth, maxWidth);
        totalTwos += std::pow(2.0L, static_cast<long double>(maxWidth));
        totalWays += std::pow(2.5L, static_cast<long double>(maxWidth));
        if (static_cast<int>(widthDistribution.size()) <= maxWidth)
            widthDistribution.resize(maxWidth + 1, 0);
        ++widthDistribution[maxWidth];
    }

    long double averageMaxWidth() const {
        return static_cast<long double>(totalMaxWidth) / legal;
    }

    long double averageTwos() const { return totalTwos / legal; }

    long double averageWays() const { return totalWays / legal; }

    void print(const char* name) const {
        std::cout << "  " << name << ": legal=" << legal
                  << ", average-max-width=" << averageMaxWidth()
                  << ", max-max-width=" << greatestWidth
                  << ", average-2^max-width=" << averageTwos()
                  << ", average-2.5^max-width=" << averageWays() << "\n";
        for (int width = 0; width < static_cast<int>(widthDistribution.size()); ++width)
            std::cout << "    " << width << ": " << widthDistribution[width] << '\n';
    }
};

// 只采样真实对局中的 Structure component；两种抛光成品共用同一个 graph、
// 同一个展开基序（lexBfsOrder）与同一个直径端点 init。
inline void testDistributionOrders(Rng& rng, const TestConfig& config) {
    long long positions = 0;
    long long games = 0;
    long long components = 0;
    OrderStats lexBfsPolishedStats;
    OrderStats lexBfsWindow3Stats;
    int greatestWidth = -1;
    const char* greatestAlgorithm = nullptr;
    ObservedBoard greatestWidthBoard;
    while (positions < config.expectedPositions && counters().failures == 0) {
        ++games;
        generateGame(config, rng, [&](const Snapshot& snapshot) {
            if (positions == config.expectedPositions || counters().failures != 0) return;

            for (const Structure::Instance& instance : snapshot.analysis.structure.components) {
                const auto graph = OrderProbe::makeGreedyOrderGraph(*instance.shape);
                const int n = OrderProbe::boxCount(graph);
                const BoxId init = OrderProbe::start(graph);
                auto check = [&](const char* name, const std::vector<BoxId>& order, OrderStats& stats,
                                 int& maxWidth) {
                    ++counters().checks;
                    if (!OrderProbe::checkOrder(graph, init, order, maxWidth)) {
                        ++counters().failures;
                        std::cerr << "[FAIL] distribution/" << name << ": boxes=" << n
                                  << "\n  order:";
                        for (BoxId box : order) std::cerr << ' ' << box;
                        std::cerr << '\n';
                        logerr("invalid connected expansion order", snapshot.game.board,
                               &snapshot.analysis.basic, &snapshot.analysis.structure);
                        return false;
                    }
                    stats.note(maxWidth);
                    return true;
                };
                int maxWidth = 0;
                const std::vector<BoxId> lexBfsPolishedOrder =
                    OrderProbe::lexBfsPolished(graph);
                if (!check("lex-bfs-polished", lexBfsPolishedOrder, lexBfsPolishedStats,
                           maxWidth)) return;
                if (maxWidth > greatestWidth) {
                    greatestWidth = maxWidth;
                    greatestAlgorithm = "lex-bfs-polished";
                    greatestWidthBoard = snapshot.game.board;
                }
                const std::vector<BoxId> lexBfsWindow3Order =
                    OrderProbe::lexBfsWindow3(graph);
                if (!check("lex-bfs-window3", lexBfsWindow3Order, lexBfsWindow3Stats,
                           maxWidth)) return;
                if (maxWidth > greatestWidth) {
                    greatestWidth = maxWidth;
                    greatestAlgorithm = "lex-bfs-window3";
                    greatestWidthBoard = snapshot.game.board;
                }

                ++components;
            }
            ++positions;
            if (positions % 10000 == 0)
                std::cout << "distribution/order-width-progress: positions=" << positions
                          << ", lex-bfs-polished-average-width="
                          << lexBfsPolishedStats.averageMaxWidth()
                          << ", lex-bfs-polished-average-2="
                          << lexBfsPolishedStats.averageTwos()
                          << ", lex-bfs-polished-average-2.5="
                          << lexBfsPolishedStats.averageWays()
                          << ", lex-bfs-window3-average-width="
                          << lexBfsWindow3Stats.averageMaxWidth()
                          << ", lex-bfs-window3-average-2=" << lexBfsWindow3Stats.averageTwos()
                          << ", lex-bfs-window3-average-2.5="
                          << lexBfsWindow3Stats.averageWays()
                          << "\n" << std::flush;
        });
    }
    std::cout << "distribution/order-width: " << positions << " positions from "
              << games << " games, " << components << " components\n";
    lexBfsPolishedStats.print("lex-bfs-polished");
    lexBfsWindow3Stats.print("lex-bfs-window3");
    std::cout << "distribution/max-width-board: algorithm=" << greatestAlgorithm
              << ", width=" << greatestWidth << '\n';
    printOrderWidthBoard(greatestWidthBoard);
}

}  // namespace mss::test
