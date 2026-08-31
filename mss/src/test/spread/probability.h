#pragma once

#include "test/common.h"

namespace mss::test {

inline void testProbabilitySpread(Rng& rng, int iter) {
    GameConfig config;
    for (int i = 0; i < iter; ++i) {
        generateGame(config, rng, [](const Snapshot& s) {
            if (counters().failures != 0) return;
            const auto& board = s.game.board;
            const auto& basic = s.analysis.basic;
            const auto& structure = s.analysis.structure;
            const auto& probability = s.analysis.probability;
            MSS_TEST_CHECK(probability.candidates > 0.0L, "exact analysis has no candidates", board, &basic, &structure);
            long double expectedMines = 0.0L;
            for (int x = 1; x <= board.rows; ++x) for (int y = 1; y <= board.cols; ++y) {
                if (board.board[x][y] != Cell::Hidden) continue;
                const CellId cell = board.id(x, y);
                const CellLocation loc = structure.cellLoc[static_cast<std::size_t>(cell)];
                const long double raw = loc.component == -1
                    ? (basic.marks[x][y] == Basic::Mark::Mine ? 1.0L
                       : basic.marks[x][y] == Basic::Mark::Unknown ? probability.tCellProbability : 0.0L)
                    : loc.box == -1 ? 0.0L
                    : probability.components[static_cast<std::size_t>(loc.component)].boxProbs[
                        static_cast<std::size_t>(loc.box)];
                const long double p = probability.mineProbability(cell, board, basic, structure);
                probabilityExtremes().note(raw);
                if (!(p >= 0.0L && p <= 1.0L)) {
#if defined(MSS_TEST_RAW_PROBABILITIES)
                    ++counters().checks;
#else
                    ++counters().checks;
                    ++counters().failures;
                    std::cerr << "probability at (" << x << ',' << y << ") = "
                              << std::hexfloat << p << std::defaultfloat << '\n';
                    logerr("mine probability is outside [0, 1]", board, &basic, &structure);
                    return;
#endif
                }
                ++counters().checks;
#if !defined(MSS_TEST_RAW_PROBABILITIES)
                if (basic.marks[x][y] == Basic::Mark::Safe)
                    MSS_TEST_CHECK(p == 0.0L, "logically safe cell is not exactly probability zero",
                                   board, &basic, &structure);
                if (basic.marks[x][y] == Basic::Mark::Mine)
                    MSS_TEST_CHECK(p == 1.0L, "logically mined cell is not exactly probability one",
                                   board, &basic, &structure);
                if (p == 0.0L)
                    MSS_TEST_CHECK(!s.game.mine(x, y), "probability-zero cell is a real mine",
                                   board, &basic, &structure);
                if (p == 1.0L)
                    MSS_TEST_CHECK(s.game.mine(x, y), "probability-one cell is actually safe",
                                   board, &basic, &structure);
#endif
                expectedMines += raw;
            }
            MSS_TEST_CHECK(std::fabs(expectedMines - static_cast<long double>(board.totalMines)) < 1e-9L,
                "mine-probability sum differs from total mine count", board, &basic, &structure);
            MSS_TEST_CHECK(s.next.x != 0, "unfinished board has no selectable hidden cell", board, &basic, &structure);
        });
    }
}

}  // namespace mss::test
