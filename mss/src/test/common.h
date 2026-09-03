#pragma once

// ═════════════════════════════════════════════════════════════════════
// test/ 架构规范 —— 改动或新增 test 代码之前必读（含 AI）。
//
// 1. 模块形态：每个测试模块 = 一个
//    inline void testXxx(const unsigned long long& seed, const TestConfig&)，
//    零其他对外符号（探针等私有辅助除外）；harness.cpp 按声明顺序调用。
//    seed 在 harness.cpp 中声明、永久不变；模块内部各自 Rng rng(seed) 起流
//    （各模块独立流 ⇒ 输出不依赖启用矩阵，单模块与全量运行结果一致）。
// 2. 启用方式：harness.cpp 的 main 里用注释开关控制，一行一个模块；
//    禁止发明命令行参数 / 模块表 / 动态注册等替代机制（人手注释最可靠）。
// 3. 配置纪律：TestConfig 不留默认值，全部字段在 harness.cpp 显式写出；
//    seconds / games 二选一驱动，另一个为 -1；双 -1 由 runGames 拒绝。
// 4. 统一驱动：真实对局测试一律经 runGames()（时间盒 + 局数二选一、
//    失败即停、每局面回调一次 Snapshot）。禁止复制 while 循环。
//    例外（决策器或分析路径与 generateGame 不同，如只测分布层吞吐）必须
//    自建循环并在文件头写明理由。
// 5. 输出纪律：每模块结束时输出一行汇总，前缀 = 模块路径名
//    （performance/…、basic/…），内容自定但须含位置数/局数与耗时；
//    专项指纹（move-hash）可例外，须注明。
// 6. 失败语义：断言一律 MSS_TEST_CHECK（计入 counters()），失败即打印
//    [FAIL] 并返回；runGames 见 counters().failures 即整体停止。
// 7. 随机源：一律 mss::test::Rng，由入参 seed 在模块内部构造（Rng rng(seed)）；
//    禁止引入其他随机源，禁止模块间共享随机流（跨模块共享 = 输出依赖启用顺序）。
// 8. 共享纪律：Game / Analysis / Snapshot / TimeBox / runGames 等一律复用
//    本文件；禁止复制实现（历史上复制粘贴是 harness 不可读的根源）。
// ═════════════════════════════════════════════════════════════════════

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <string_view>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution/distribution.h"
#include "analysis/distribution/distribution_graph.h"
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

inline void logerr(std::string_view message) {
    std::cerr << "\n[FAIL] " << message << '\n';
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
                      << " constraints=" << c.shape->constraintCount() << '\n';
        }
    }
}

#define MSS_TEST_CHECK(condition, message, board, basic, structure) \
    do { ++::mss::test::counters().checks; if (!(condition)) { \
        ++::mss::test::counters().failures; \
        ::mss::test::logerr((message), (board), (basic), (structure)); return; \
    } } while (false)

enum class PositionFilter { All, GuessOnly };
// 分布层实现选择：Old = 旧 Distribution::Solver；Graph = 图算法实现。
enum class DistributionMode { Old, Graph };
// 统一时间盒：跑满时间盒即到期。seconds < 0 表示不限时（局数驱动模式，
// expired() 恒为 false，终止由调用方的局数条件负责）。
struct TimeBox {
    std::chrono::steady_clock::time_point start;
    std::chrono::steady_clock::time_point deadline;

    explicit TimeBox(double seconds)
        : start(std::chrono::steady_clock::now()),
          deadline(seconds < 0
                       ? std::chrono::steady_clock::time_point::max()
                       : start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                     std::chrono::duration<double>(seconds))) {}

    bool expired() const { return std::chrono::steady_clock::now() >= deadline; }
    double elapsedSeconds() const {
        return std::chrono::duration<double>(
                   std::chrono::steady_clock::now() - start)
            .count();
    }
};

// 所有真实对局测试共用的采样配置。**不留默认值**：harness main 必须显式写出
// 全部字段（方便手动改参数）。驱动量二选一：
//   seconds >= 0 → 时间盒驱动；games >= 0 → 局数驱动；
//   其中一个为 -1 表示由另一个控制；两者都为 -1 是配置错误（runGames 拒绝）。
struct TestConfig {
    int rows;
    int cols;
    int mines;
    double seconds;              // >=0：时间盒（秒）；-1：由 games 控制
    int games;                   // >=0：游戏局数；-1：由 seconds 控制
    PositionFilter filter;       // All：全部局面；GuessOnly：只留必须猜的局面
    DistributionMode distributionMode;
    int maxRestarts;             // requireWinningGame 时生成一条可赢对局的重试上限
    bool requireWinningGame;
    bool firstMoveSafe;
};

struct Game {
    ObservedBoard board;
    std::vector<char> mines;
    int opened = 0;
    explicit Game(const TestConfig& c) : board(c.rows, c.cols, c.mines),
        mines(c.rows * c.cols, 0) {}
    int flat(int x, int y) const { return (x - 1) * board.cols + (y - 1); }
    bool mine(int x, int y) const { return mines[flat(x, y)] != 0; }
    void placeMines(Rng& rng, bool firstMoveSafe) {
        std::vector<int> cells(board.rows * board.cols);
        const int first = firstMoveSafe ? 1 : 0;
        for (int i = first; i < static_cast<int>(cells.size()); ++i)
            cells[i - first] = i;
        cells.resize(cells.size() - first);
        for (int i = static_cast<int>(cells.size()) - 1; i > 0; --i)
            std::swap(cells[i], cells[rng.below(i + 1)]);
        for (int i = 0; i < board.totalMines; ++i) mines[cells[i]] = 1;
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

// Graph 模式桥接：把图算法分布装进旧池（Distribution::DistPool）。Exact 的
// 取数路径不变（Solver::analyze → pool.get 缓存优先），命中即 graph 数据，
// 旧 DFS 不运行——分析层零改动，开关完全由 test 层控制。
inline void bridgeGraphDistributions(const Structure::Result& structure,
                                     DistributionSolver::DistPool& graphPool,
                                     Distribution::DistPool& oldPool) {
    for (const Structure::Instance& inst : structure.components) {
        if (oldPool.get(inst.shape) != nullptr) continue;  // 已桥接（同 shape 幂等）
        const DistributionSolver::Distribution* g =
            DistributionSolver::analyze(*inst.shape, graphPool);
        Distribution legacy;
        legacy.entries.reserve(g->entries.size());
        for (const DistributionSolver::Distribution::Entry& e : g->entries) {
            Distribution::Entry outEntry;
            outEntry.mineCount = e.mineCount;
            outEntry.ways = e.ways;
            outEntry.perBoxExpectation = e.perBoxExpectation;
            legacy.entries.push_back(std::move(outEntry));
        }
        oldPool.insert(inst.shape, std::move(legacy));
    }
}

struct Analysis {
    Basic::Result basic;
    Structure::ShapePool shapes;
    Structure::Result structure;
    Distribution::DistPool distributions;            // 旧实现池（Graph 模式时为桥接容器）
    DistributionSolver::DistPool graphDistributions; // 图算法分布池
    DistributionMode distributionMode;               // 主路径分布实现选择
    Probability::Result probability;
    explicit Analysis(const ObservedBoard& b, DistributionMode mode)
        : basic(Basic::analyze(b)),
          structure(Structure::analyze(b, basic, shapes)),
          distributionMode(mode) {
        if (distributionMode == DistributionMode::Graph)
            bridgeGraphDistributions(structure, graphDistributions, distributions);
        probability = Exact::analyze(b, basic, structure, distributions);
    }

    void update(const ObservedBoard& board, const Basic::Delta& updates) {
        Basic::update(board, basic, updates);
        Structure::update(board, basic, structure, shapes, updates);
        if (distributionMode == DistributionMode::Graph)
            bridgeGraphDistributions(structure, graphDistributions, distributions);
        probability = Exact::analyze(board, basic, structure, distributions);
    }
};

struct Move { int x = 0; int y = 0; long double mineProbability = 1.0L; };
inline Move lowestRiskMove(const ObservedBoard& board, const Analysis& analysis) {
    using Mark = Basic::Mark;
    // 原来对全盘每个 Hidden 格调 mineProbability（O(nm) 次查询）。实际只需：
    //   - Safe 格（p=0）：全局最优，坐标序第一个即返回（与原语义一致）；
    //   - 前沿格：frontierCells 按块枚举，概率直接取自 boxProbs，零逐格查询；
    //   - 其余 Unknown 格：概率统一 = tCellProbability（probability.h:
    //     mineProbability 对 Unknown 恒返回 tCellProbability），只需记住坐标序
    //     第一个作兜底。
    int fallbackX = 0;
    int fallbackY = 0;
    for (int x = 1; x <= board.rows; ++x)
        for (int y = 1; y <= board.cols; ++y) {
            if (board.board[x][y] != Cell::Hidden) continue;
            const Mark mark = analysis.basic.marks[x][y];
            if (mark == Mark::Safe) return Move{x, y, 0.0L};
            if (mark == Mark::Unknown && fallbackX == 0) {
                fallbackX = x;
                fallbackY = y;
            }
        }

    Move out;  // mineProbability = 1.0L；无前沿格时由兜底承担
    analysis.probability.frontierCells(
        board, analysis.structure,
        [&](int x, int y, long double prob) {
            if (prob < out.mineProbability) out = Move{x, y, prob};
        });
    if (fallbackX != 0 && out.mineProbability >= analysis.probability.tCellProbability)
        out = Move{fallbackX, fallbackY, analysis.probability.tCellProbability};
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
        Analysis analysis(game.board, config.distributionMode);
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

// ── 统一对局驱动（规范 R4）──
// 真实对局测试一律经 runGames() 生成对局并消费 Snapshot；禁止各自复制
// while 循环（曾出现五份复制、时间盒/失败检查条件各不相同的乱象）。
// 规则：
//   - 驱动量按 TestConfig：seconds / games 二选一（另一个 -1）；双 -1 拒绝；
//   - 时间盒到期或有断言失败（counters().failures）立即停止；
//   - perSnapshot 只做本模块的统计与断言，不再自行检查时间盒/失败。
struct RunSummary {
    long long games = 0;        // 已完成局数
    double elapsedSeconds = 0;  // 驱动实际耗时（时间盒到期或达到局数时）
};

template <typename Fn>
inline RunSummary runGames(const TestConfig& config, Rng& rng, Fn&& perSnapshot) {
    if (config.seconds < 0 && config.games < 0) {
        logerr("TestConfig: seconds 与 games 同为 -1，必须二选一指定驱动量");
        std::abort();
    }
    TimeBox timebox(config.seconds);
    RunSummary summary;
    while ((config.games < 0 || summary.games < config.games) &&
           !timebox.expired() && counters().failures == 0) {
        ++summary.games;
        generateGame(config, rng, [&](const Snapshot& snapshot) {
            if (timebox.expired() || counters().failures != 0) return;
            perSnapshot(snapshot);
        });
    }
    summary.elapsedSeconds = timebox.elapsedSeconds();
    return summary;
}

}  // namespace mss::test
