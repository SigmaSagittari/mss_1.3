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
#include "analysis/distribution/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/structure.h"

namespace mss::test {

// 测试专用 xorshift64*（与 core 的 Rng/splitmix64 无关）；固定种子可复现。
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

// 收集观测到的雷概率极值，便于复现/定位浮点边界问题。
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
// 所有真实对局测试共用的采样配置。每个测试只额外声明自身特有的参数。
struct TestConfig {
    int rows = 16;
    int cols = 30;
    int mines = 99;
    long long expectedPositions = 10000;
    PositionFilter filter = PositionFilter::All;  // All：全部局面；GuessOnly：只留必须猜的局面
    int maxRestarts = 10000;                      // requireWinningGame 时生成一条可赢对局的重试上限
    bool requireWinningGame = false;
    bool firstMoveSafe = false;
};

struct Game {
    ObservedBoard board;
    std::vector<char> mines;
    int opened = 0;
    explicit Game(const TestConfig& c) : board(c.rows, c.cols, c.mines),
        mines(static_cast<std::size_t>(c.rows * c.cols), 0) {}
    int flat(int x, int y) const { return (x - 1) * board.cols + (y - 1); }
    bool mine(int x, int y) const { return mines[static_cast<std::size_t>(flat(x, y))] != 0; }
    void placeMines(Rng& rng, bool firstMoveSafe) {
        std::vector<int> cells(static_cast<std::size_t>(board.rows * board.cols));
        const int first = firstMoveSafe ? 1 : 0;
        for (int i = first; i < static_cast<int>(cells.size()); ++i)
            cells[static_cast<std::size_t>(i - first)] = i;
        cells.resize(cells.size() - first);
        for (int i = static_cast<int>(cells.size()) - 1; i > 0; --i)
            std::swap(cells[static_cast<std::size_t>(i)], cells[static_cast<std::size_t>(rng.below(i + 1))]);
        for (int i = 0; i < board.totalMines; ++i) mines[static_cast<std::size_t>(cells[i])] = 1;
    }
    int adjacentMines(int x, int y) const {
        int total = 0;
        forEachAdjacent(x, y, board.rows, board.cols, [&](int nx, int ny) { total += mine(nx, ny); });
        return total;
    }
    bool reveal(int x, int y, Basic::Delta& updates) {
        if (mine(x, y)) return false;
        // 翻出 0 则连锁泛洪（BFS 展开整个空区）。
        std::deque<std::pair<int, int>> pending{{x, y}};
        while (!pending.empty()) {
            const auto [cx, cy] = pending.front(); pending.pop_front();
            if (board.board[cx][cy] != Cell::Hidden || mine(cx, cy)) continue;
            const int digit = adjacentMines(cx, cy);
            board.board[cx][cy] = static_cast<Cell>(digit); ++opened;
            updates.upd.push_back(
                Basic::Delta::updateCell{board.id(cx, cy), static_cast<Cell>(digit)});
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
    explicit Analysis(const ObservedBoard& b) : basic(Basic::analyze(b)),
        structure(Structure::analyze(b, basic, shapes)),
        probability(Exact::analyze(b, basic, structure, distributions)) {}

    void update(const ObservedBoard& board, const Basic::Delta& updates) {
        Basic::update(board, basic, updates);
        Structure::update(board, basic, structure, shapes, updates);
        probability = Exact::analyze(board, basic, structure, distributions);
    }
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

// 随机雷局按"最低雷概率"策略游玩；默认采样每个已经历过的真实局面，输局
// 只会终止当前对局。requireWinningGame 时才丢弃输局并重开；数字 0 处连锁翻开。
template <typename Fn>
inline void generateGame(const TestConfig& config, Rng& rng, Fn&& consume) {
    if (config.rows <= 0 || config.cols <= 0 || config.mines < 0 ||
        config.mines >= config.rows * config.cols) std::abort();
    for (int restart = 0; restart < config.maxRestarts; ++restart) {
        Game game(config); game.placeMines(rng, config.firstMoveSafe); bool lost = false;
        if (config.firstMoveSafe) {
            Basic::Delta firstMove;
            game.reveal(1, 1, firstMove);
        }
        Analysis analysis(game.board);
        while (!game.won()) {
            const Move move = lowestRiskMove(game.board, analysis);
            Basic::Delta updates;
            if (move.x == 0 || !game.reveal(move.x, move.y, updates)) { lost = true; break; }
            if (game.won()) break;
            analysis.update(game.board, updates);
            const Move next = lowestRiskMove(game.board, analysis);
            const Snapshot snapshot{game, analysis, next, next.mineProbability > 1e-15L};
            if (config.filter == PositionFilter::All || snapshot.mustGuess) consume(snapshot);
        }
        if ((!lost && game.won()) || !config.requireWinningGame) return;
    }
    std::cerr << "could not generate a winning game after " << config.maxRestarts << " restarts\n";
    std::abort();
}

}  // namespace mss::test
