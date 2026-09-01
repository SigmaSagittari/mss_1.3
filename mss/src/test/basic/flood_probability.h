#pragma once

#include <iostream>

#include "test/common.h"

namespace mss::test {

// 泛洪前的旧版 basic，仅用于为新版 basic 的 soundness 测试提供较快的独立基准。
struct LegacyBasic {
    static Basic::Result analyze(const ObservedBoard& state) {
        using Mark = Basic::Mark;
        Basic::Result result;
        result.rows = state.rows;
        result.cols = state.cols;
        result.marks.resize(state.rows, state.cols, Mark::Safe);
        const auto& board = state.board;
        const int rows = state.rows;
        const int cols = state.cols;

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (board[x][y] == Cell::Hidden)
                    result.marks[x][y] = Mark::Unknown;

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (isNumber(board[x][y]))
                    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                        if (board[nx][ny] == Cell::Hidden)
                            result.marks[nx][ny] = Mark::Frontier;
                    });

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (isNumber(board[x][y])) {
                    int hiddenCount = 0;
                    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                        if (board[nx][ny] == Cell::Hidden) ++hiddenCount;
                    });
                    if (hiddenCount == numberValue(board[x][y]))
                        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                            if (board[nx][ny] == Cell::Hidden)
                                result.marks[nx][ny] = Mark::Mine;
                        });
                }

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (isNumber(board[x][y])) {
                    int mineCount = 0;
                    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                        if (result.marks[nx][ny] == Mark::Mine) ++mineCount;
                    });
                    if (mineCount == numberValue(board[x][y]))
                        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                            if (board[nx][ny] == Cell::Hidden &&
                                result.marks[nx][ny] != Mark::Mine)
                                result.marks[nx][ny] = Mark::Safe;
                        });
                }

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y)
                if (isNumber(board[x][y])) {
                    int mineCount = 0, openCount = 0;
                    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
                        if (result.marks[nx][ny] == Mark::Mine) ++mineCount;
                        if (board[nx][ny] == Cell::Hidden &&
                            result.marks[nx][ny] != Mark::Safe)
                            ++openCount;
                    });
                    const int value = numberValue(board[x][y]);
                    if (value < mineCount || value > openCount) {
                        result.valid = false;
                        break;
                    }
                }

        for (int x = 1; x <= rows; ++x)
            for (int y = 1; y <= cols; ++y) {
                if (result.marks[x][y] == Mark::Mine) ++result.mineSum;
                if (result.marks[x][y] == Mark::Unknown) ++result.unknownSum;
            }

        return result;
    }
};

// 真实对局中的 soundness 检验：旧 basic 计算的概率是独立基准。新版 basic
// 可以漏掉概率恰为 0/1 的格子，但其推导出的 Safe / Mine 必须分别对应 0 / 1。
inline void testBasicFloodProbability(
    Rng& rng, const TestConfig& config) {
    long long positions = 0;
    long long games = 0;
    while (positions < config.expectedPositions && counters().failures == 0) {
        ++games;
        generateGame(config, rng, [&](const Snapshot& snapshot) {
            if (positions == config.expectedPositions || counters().failures != 0) return;

            const ObservedBoard& board = snapshot.game.board;
            const Basic::Result legacyBasic = LegacyBasic::analyze(board);
            const Basic::Result& floodBasic = snapshot.analysis.basic;
            Structure::ShapePool shapes;
            const Structure::Result legacyStructure =
                Structure::analyze(board, legacyBasic, shapes);
            Distribution::DistPool distributions;
            const Probability::Result probability =
                Exact::analyze(board, legacyBasic, legacyStructure, distributions);
            MSS_TEST_CHECK(probability.candidates > 0.0L,
                           "legacy basic exact analysis has no candidates",
                           board, &legacyBasic, &legacyStructure);

            for (int x = 1; x <= board.rows; ++x)
                for (int y = 1; y <= board.cols; ++y) {
                    if (board.board[x][y] != Cell::Hidden) continue;
                    const Basic::Mark mark = floodBasic.marks[x][y];
                    if (mark != Basic::Mark::Safe && mark != Basic::Mark::Mine) continue;
                    const long double p = probability.mineProbability(
                        board.id(x, y), board, legacyBasic, legacyStructure);
                    if (mark == Basic::Mark::Safe)
                        MSS_TEST_CHECK(p == 0.0L,
                                       "flood basic reports a nonzero-probability cell as safe",
                                       board, &floodBasic, nullptr);
                    else
                        MSS_TEST_CHECK(p == 1.0L,
                                       "flood basic reports a non-unit-probability cell as mine",
                                       board, &floodBasic, nullptr);
                }
            ++positions;
        });
    }

    std::cout << "basic/flood-probability: " << positions << " positions from "
              << games << " games\n";
}

}  // namespace mss::test
