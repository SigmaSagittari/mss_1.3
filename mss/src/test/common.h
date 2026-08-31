#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string_view>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"

namespace mss::test {

struct Rng {
    std::uint64_t state;
    explicit Rng(std::uint64_t seed = 0x9e3779b97f4a7c15ULL) : state(seed) {}
    std::uint64_t next() {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        return state * 0x2545f4914f6cdd1dULL;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
};

struct Counters { long long checks = 0; long long failures = 0; };
inline Counters& counters() { static Counters value; return value; }

struct ProbabilityExtremes {
    long double minPositive = 1.0L;
    long double maxBelowOne = 0.0L;
    long double minObserved = 1.0L;
    long double maxObserved = 0.0L;
    long double maxBelowOneMinus1e10 = 0.0L;
    long long positiveSamples = 0;
    void note(long double p) {
        minObserved = std::min(minObserved, p);
        maxObserved = std::max(maxObserved, p);
        if (p < 1.0L - 1e-10L) maxBelowOneMinus1e10 = std::max(maxBelowOneMinus1e10, p);
        if (p > 0.0L && p < 1.0L) {
            minPositive = std::min(minPositive, p);
            maxBelowOne = std::max(maxBelowOne, p);
            ++positiveSamples;
        }
    }
};
inline ProbabilityExtremes& probabilityExtremes() { static ProbabilityExtremes value; return value; }

inline void printProbabilityExtremes() {
    const ProbabilityExtremes& e = probabilityExtremes();
    if (e.positiveSamples == 0) {
        std::cout << "probability extremes: no strictly interior probabilities\n";
        return;
    }
    std::cout << "probability extremes: min positive=" << std::hexfloat << e.minPositive
              << ", max below one=" << e.maxBelowOne << ", observed=[" << e.minObserved
              << ", " << e.maxObserved << "], max p < 1-1e-10="
              << e.maxBelowOneMinus1e10 << std::defaultfloat << " ("
              << e.positiveSamples << " interior samples)\n";
}

inline void logerr(std::string_view message, const ObservedBoard& board,
                   const Basic::Result* basic = nullptr,
                   const Structure::Result* structure = nullptr) {
    std::cerr << "\n[FAIL] " << message << "\nboard " << board.rows << 'x' << board.cols
              << " mines=" << board.totalMines << '\n';
    for (int x = 1; x <= board.rows; ++x) {
        for (int y = 1; y <= board.cols; ++y) {
            const Cell cell = board.board[x][y];
            std::cerr << (cell == Cell::Hidden ? '#' : static_cast<char>('0' + numberValue(cell)));
            if (basic) {
                const char mark[] = {'s', 'm', 'f', 'u'};
                std::cerr << mark[static_cast<int>(basic->marks[x][y])];
            }
            std::cerr << ' ';
        }
        std::cerr << '\n';
    }
    if (structure) {
        std::cerr << "components=" << structure->components.size() << '\n';
        for (std::size_t i = 0; i < structure->components.size(); ++i) {
            const auto& c = structure->components[i];
            std::cerr << "  component " << i << ": boxes=" << c.boxes.count()
                      << " constraints=" << c.shape->constraints.size() << '\n';
        }
    }
}

#define MSS_TEST_CHECK(condition, message, board, basic, structure) \
    do { ++::mss::test::counters().checks; if (!(condition)) { \
        ++::mss::test::counters().failures; \
        ::mss::test::logerr((message), (board), (basic), (structure)); return; \
    } } while (false)

enum class PositionFilter { All, GuessOnly };
struct GameConfig {
    int rows = 16;
    int cols = 30;
    int mines = 99;
    PositionFilter filter = PositionFilter::All;
    int maxRestarts = 10000;
};

struct Game {
    ObservedBoard board;
    std::vector<char> mines;
    int opened = 0;
    explicit Game(const GameConfig& c) : board(c.rows, c.cols, c.mines),
        mines(static_cast<std::size_t>(c.rows * c.cols), 0) {}
    int flat(int x, int y) const { return (x - 1) * board.cols + (y - 1); }
    bool mine(int x, int y) const { return mines[static_cast<std::size_t>(flat(x, y))] != 0; }
    void placeMines(Rng& rng) {
        std::vector<int> cells(static_cast<std::size_t>(board.rows * board.cols));
        for (int i = 0; i < static_cast<int>(cells.size()); ++i) cells[static_cast<std::size_t>(i)] = i;
        for (int i = static_cast<int>(cells.size()) - 1; i > 0; --i)
            std::swap(cells[static_cast<std::size_t>(i)], cells[static_cast<std::size_t>(rng.below(i + 1))]);
        for (int i = 0; i < board.totalMines; ++i) mines[static_cast<std::size_t>(cells[i])] = 1;
    }
    int adjacentMines(int x, int y) const {
        int total = 0;
        forEachAdjacent(x, y, board.rows, board.cols, [&](int nx, int ny) { total += mine(nx, ny); });
        return total;
    }
    bool reveal(int x, int y) {
        if (mine(x, y)) return false;
        std::deque<std::pair<int, int>> pending{{x, y}};
        while (!pending.empty()) {
            const auto [cx, cy] = pending.front(); pending.pop_front();
            if (board.board[cx][cy] != Cell::Hidden || mine(cx, cy)) continue;
            const int digit = adjacentMines(cx, cy);
            board.board[cx][cy] = static_cast<Cell>(digit); ++opened;
            if (digit == 0) forEachAdjacent(cx, cy, board.rows, board.cols,
                [&](int nx, int ny) { pending.emplace_back(nx, ny); });
        }
        return true;
    }
    bool won() const { return opened == board.rows * board.cols - board.totalMines; }
};

struct Analysis {
    Basic::Result basic;
    Structure::ShapePool shapes;
    Structure::Result structure;
    Distribution::DistPool distributions;
    Probability::Result probability;
    explicit Analysis(const ObservedBoard& b) : basic(Basic::Analyzer::analyze(b)),
        structure(Structure::Analyzer::analyze(b, basic, shapes)),
        probability(Exact::analyze(b, basic, structure, distributions)) {}
};

struct Move { int x = 0; int y = 0; long double mineProbability = 1.0L; };
inline Move lowestRiskMove(const ObservedBoard& board, const Analysis& analysis) {
    Move out;
    for (int x = 1; x <= board.rows; ++x) for (int y = 1; y <= board.cols; ++y) {
        if (board.board[x][y] != Cell::Hidden) continue;
        const long double p = analysis.probability.mineProbability(board.id(x, y), board,
            analysis.basic, analysis.structure);
        if (p < out.mineProbability) out = Move{x, y, p};
    }
    return out;
}

struct Snapshot { const Game& game; const Analysis& analysis; Move next; bool mustGuess = false; };

// Random mine layouts are played by the exact minimum-risk policy. Losing layouts are discarded,
// hence every emitted position lies on a complete winning game; zeroes are flood-revealed.
template <typename Fn>
inline void generateGame(const GameConfig& config, Rng& rng, Fn&& consume) {
    if (config.rows <= 0 || config.cols <= 0 || config.mines < 0 ||
        config.mines >= config.rows * config.cols) std::abort();
    for (int restart = 0; restart < config.maxRestarts; ++restart) {
        Game game(config); game.placeMines(rng); bool lost = false;
        while (!game.won()) {
            Analysis before(game.board);
            const Move move = lowestRiskMove(game.board, before);
            if (move.x == 0 || !game.reveal(move.x, move.y)) { lost = true; break; }
            if (game.won()) break;
            Analysis after(game.board);
            const Move next = lowestRiskMove(game.board, after);
            const Snapshot snapshot{game, after, next, next.mineProbability > 1e-15L};
            if (config.filter == PositionFilter::All || snapshot.mustGuess) consume(snapshot);
        }
        if (!lost && game.won()) return;
    }
    std::cerr << "could not generate a winning game after " << config.maxRestarts << " restarts\n";
    std::abort();
}

}  // namespace mss::test
