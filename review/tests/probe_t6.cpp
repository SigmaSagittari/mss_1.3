// probe_t6.cpp — 概率网格实测探针（一次性验证，非测试套件）。
//
// 回答三个问题：
//   Q1  dump 盘面（dump_t6.txt）在「可信概率网格」（probability.h 的
//       mineProbability getter）下手算的每格概率是什么？必雷格是否被精确排除？
//   Q2  同一盘面：0.5 网格（T6 喂的垃圾）vs 可信网格，lib 输出差异？
//   Q3  随机 300 盘：Exact 可信网格 / ref::aggregate 诚实网格 / 0.5 网格，
//       三者的 lib-vs-naive 分歧计数；浮点边缘普查（boxProbs 接近 1.0 / >1.0 /
//       basic.Mine 格的值 ≠ 1.0L）。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

#include "analysis/basic.h"
#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/config.h"
#include "core/types.h"

#include "../harness/ref_mining.h"

using namespace mss;

static Cell toCell(int v) { return v < 0 ? Cell::Hidden : static_cast<Cell>(v); }

static ObservedBoard toLibBoard(const ref::RefBoard& r) {
    ObservedBoard b(r.rows, r.cols, r.totalMines);
    for (int i = 1; i <= r.rows; ++i)
        for (int j = 1; j <= r.cols; ++j) b.board[i][j] = toCell(r.at(i, j));
    return b;
}

struct Gen {
    ref::Rng rng;
    explicit Gen(std::uint64_t seed) : rng(seed) {}
};

static ref::RefBoard genConsistent(Gen& g, int rows, int cols, int mineCount,
                                   int extraHiddenMax) {
    ref::RefBoard b(rows, cols, mineCount);
    std::vector<std::pair<int, int>> all;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) all.emplace_back(i, j);
    std::shuffle(all.begin(), all.end(), std::mt19937(g.rng.u32()));
    std::vector<char> mine(static_cast<std::size_t>(rows * cols), 0);
    for (int k = 0; k < mineCount; ++k) {
        const int f = b.flat(all[k].first, all[k].second);
        mine[static_cast<std::size_t>(f)] = 1;
        b.trueMines.push_back(f);
    }
    std::vector<std::pair<int, int>> nonMines;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (!mine[static_cast<std::size_t>(b.flat(i, j))]) nonMines.emplace_back(i, j);
    std::shuffle(nonMines.begin(), nonMines.end(), std::mt19937(g.rng.u32()));
    const int extra = std::min(static_cast<int>(nonMines.size()),
                               g.rng.below(extraHiddenMax + 1));
    std::vector<char> extraHide(static_cast<std::size_t>(rows * cols), 0);
    for (int k = 0; k < extra; ++k)
        extraHide[static_cast<std::size_t>(b.flat(nonMines[k].first, nonMines[k].second))] = 1;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) {
            if (mine[static_cast<std::size_t>(b.flat(i, j))] ||
                extraHide[static_cast<std::size_t>(b.flat(i, j))]) {
                b.at(i, j) = -1;
                continue;
            }
            int v = 0;
            ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                if (mine[static_cast<std::size_t>(b.flat(ni, nj))]) ++v;
            });
            b.at(i, j) = v;
        }
    return b;
}

// 参考求解器（harness.cpp 逐字拷贝）。
struct NaiveSolver {
    std::vector<std::vector<int>> reveal;
    int m = 0;
    int value(const std::vector<int>& configs, std::vector<char>& opened) {
        if (configs.empty()) return 0;
        if (configs.size() == 1) return 1;
        std::vector<int> deadCnt(m, 0);
        for (int ci : configs)
            for (int j = 0; j < m; ++j)
                if (revealAt(ci, j) < 0) ++deadCnt[j];
        std::vector<int> safe;
        for (int j = 0; j < m; ++j)
            if (!opened[j] && deadCnt[j] == 0) safe.push_back(j);
        if (!safe.empty()) {
            std::map<std::vector<int>, std::vector<int>> groups;
            for (int ci : configs) {
                std::vector<int> key;
                key.reserve(safe.size());
                for (int j : safe) key.push_back(revealAt(ci, j));
                groups[key].push_back(ci);
            }
            for (int j : safe) opened[j] = 1;
            int v = 0;
            for (auto& [k, grp] : groups) v += value(grp, opened);
            for (int j : safe) opened[j] = 0;
            return v;
        }
        int best = 0;
        for (int j = 0; j < m; ++j) {
            if (opened[j]) continue;
            if (deadCnt[j] == static_cast<int>(configs.size())) continue;
            std::map<int, std::vector<int>> groups;
            for (int ci : configs) {
                if (revealAt(ci, j) < 0) continue;
                groups[revealAt(ci, j)].push_back(ci);
            }
            opened[j] = 1;
            int w = 0;
            for (auto& [k, grp] : groups) w += value(grp, opened);
            opened[j] = 0;
            best = (std::max)(best, w);
        }
        return best;
    }
    int revealAt(int ci, int j) const {
        return reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)];
    }
};

// 与 harness testEndgame 相同的 per-move naive 胜数（候选集 = candCells）。
static std::vector<int> naivePerMove(const ref::RefBoard& rb,
                                     const std::vector<std::vector<int>>& ps,
                                     const std::vector<std::pair<int, int>>& candCells) {
    NaiveSolver nv;
    nv.m = static_cast<int>(candCells.size());
    nv.reveal.assign(ps.size(), std::vector<int>(static_cast<std::size_t>(nv.m), 0));
    for (std::size_t ci = 0; ci < ps.size(); ++ci) {
        std::vector<char> mk(static_cast<std::size_t>(rb.rows * rb.cols), 0);
        for (int f : ps[ci]) mk[static_cast<std::size_t>(f)] = 1;
        for (int j = 0; j < nv.m; ++j) {
            const int i = candCells[static_cast<std::size_t>(j)].first;
            const int k = candCells[static_cast<std::size_t>(j)].second;
            if (mk[static_cast<std::size_t>(rb.flat(i, k))]) {
                nv.reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)] = -1;
                continue;
            }
            int cnt = 0;
            ref::forEa(i, k, rb.rows, rb.cols, [&](int ni, int nj) {
                if (rb.at(ni, nj) < 0 && mk[static_cast<std::size_t>(rb.flat(ni, nj))]) ++cnt;
            });
            nv.reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)] = cnt;
        }
    }
    std::vector<int> allC(static_cast<std::size_t>(ps.size()));
    for (int i = 0; i < static_cast<int>(allC.size()); ++i) allC[i] = i;
    std::vector<char> opened(static_cast<std::size_t>(nv.m), 0);
    std::vector<int> wins(static_cast<std::size_t>(nv.m), 0);
    for (int j = 0; j < nv.m; ++j) {
        std::map<int, std::vector<int>> groups;
        for (int ci : allC)
            if (nv.revealAt(ci, j) >= 0) groups[nv.revealAt(ci, j)].push_back(ci);
        opened[static_cast<std::size_t>(j)] = 1;
        int w = 0;
        for (auto& [k, grp] : groups) w += nv.value(grp, opened);
        opened[static_cast<std::size_t>(j)] = 0;
        wins[static_cast<std::size_t>(j)] = w;
    }
    return wins;
}

// 给定概率网格 → 候选格（行主序，prob<1.0），与 buildCandidates 同序。
static std::vector<std::pair<int, int>> candCellsFrom(
    const ObservedBoard& b, const Grid<long double>& probGrid) {
    std::vector<std::pair<int, int>> out;
    for (int i = 1; i <= b.rows; ++i)
        for (int j = 1; j <= b.cols; ++j)
            if (b.board[i][j] == Cell::Hidden && probGrid[i][j] < 1.0L)
                out.emplace_back(i, j);
    return out;
}

static void runLibAndPrint(const char* tag, const ObservedBoard& b,
                           const Basic::Result& basic, const Structure::Result& st,
                           Distribution::DistPool& dpool, const Grid<long double>& grid,
                           const std::vector<std::pair<int, int>>& candCells,
                           const std::vector<int>& naiveWins) {
    EndgameBruteforce::Config cfg;
    cfg.checkAllMoves = true;
    const EndgameBruteforce::Result lib =
        EndgameBruteforce::solveEndgame(b, basic, st, dpool, grid, cfg);
    std::printf("  [%s] lib total=%d moves=%zu naive-m=%zu\n", tag,
                lib.totalPossibilities, lib.result.size(), candCells.size());
    for (std::size_t j = 0; j < lib.result.size(); ++j)
        std::printf("    move#%zu lib(%d,%d,w=%d) naive w=%d\n", j,
                    lib.result[j].x, lib.result[j].y, lib.result[j].wins,
                    naiveWins.empty() ? -1 : naiveWins[j]);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);

    // ── Part 1：dump_t6.txt 盘面（3x3、3 雷，basic 已证 3 雷）──
    std::printf("=== Part 1: dump_t6 board ===\n");
    {
        const int rows = 3, cols = 3, mines = 3;
        ref::RefBoard rb(rows, cols, mines);
        // 真实雷位：(1,2) (2,1) (2,2)；额外隐藏：(3,1) (3,3)
        const bool mineAt[3][3] = {{false, true,  false},
                                   {true,  true,  false},
                                   {false, false, false}};
        const bool hiddenAt[3][3] = {{false, true,  false},
                                     {true,  true,  false},
                                     {true,  false, true}};
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j) {
                if (mineAt[i - 1][j - 1]) rb.trueMines.push_back(rb.flat(i, j));
            }
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j) {
                if (mineAt[i - 1][j - 1] || hiddenAt[i - 1][j - 1]) {
                    rb.at(i, j) = -1;
                    continue;
                }
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (mineAt[ni - 1][nj - 1]) ++v;
                });
                rb.at(i, j) = v;
            }
        const ObservedBoard b = toLibBoard(rb);
        const Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool spool;
        const Structure::Result st = Structure::Analyzer::analyze(b, basic, spool);
        Distribution::DistPool dpool;
        const Probability::Result prob = Exact::analyze(b, basic, st, dpool);
        const auto ps = ref::enumeratePlacements(rb);

        std::printf("placements=%zu  lib(0.5grid) 应为 5 个候选、全 w=1（dump 复现）\n", ps.size());
        std::printf("per-cell trusted prob (probability.h getter):\n");
        Grid<long double> trusted(rows, cols, 0.0L);
        Grid<long double> half(rows, cols, 0.0L);
        for (int i = 1; i <= rows; ++i) {
            for (int j = 1; j <= cols; ++j) {
                const CellId cell = b.id(i, j);
                const long double p = prob.mineProbability(cell, b, basic, st);
                trusted[i][j] = (b.board[i][j] == Cell::Hidden) ? p : 0.0L;
                half[i][j] = (b.board[i][j] == Cell::Hidden) ? 0.5L : 0.0L;
                std::printf("  (%d,%d) board=%s mark=%d loc=(%d,%d) p=%.21Lf  p==1.0L:%s\n", i, j,
                            b.board[i][j] == Cell::Hidden ? "H" : "num",
                            static_cast<int>(basic.marks[i][j]),
                            st.cellLoc[static_cast<std::size_t>(cell)].component,
                            st.cellLoc[static_cast<std::size_t>(cell)].box, p,
                            p == 1.0L ? "true" : "FALSE");
            }
        }
        const auto candTrusted = candCellsFrom(b, trusted);
        std::printf("trusted 候选 (p<1.0):");
        for (auto [x, y] : candTrusted) std::printf(" (%d,%d)", x, y);
        std::printf("\n");

        const auto naiveT = naivePerMove(rb, ps, candTrusted);
        const auto naiveH = naivePerMove(rb, ps, candCellsFrom(b, half));
        runLibAndPrint("trusted-grid", b, basic, st, dpool, trusted, candTrusted, naiveT);
        runLibAndPrint("0.5-grid   ", b, basic, st, dpool, half, candCellsFrom(b, half), naiveH);
    }

    // ── Part 2：随机盘面三网格差分 + 浮点边缘普查 ──
    std::printf("\n=== Part 2: 300-board sweep ===\n");
    {
        Gen g(0xDEADBEEF9876ULL);
        int boards = 0;
        struct GridStat {
            const char* name;
            int boardMismatch = 0;
            int moveMismatch = 0;
        };
        GridStat stat[3] = {{"exact ", 0, 0}, {"honest", 0, 0}, {"half  ", 0, 0}};
        int basicMineBad = 0;          // basic.Mine 格 getter != 1.0L
        int floatNear1 = 0;            // 0.9999999 < v < 1.0
        long double minNear1 = 2.0L;   // 最接近 1.0（从下方）
        int floatAbove1 = 0;           // v > 1.0
        int exactVsAggDiff = 0;        // exact 与 aggregate 在隐藏格上 >1e-9 的差异数
        long double worstDiff = 0.0L;  // 最大 |pe-pa|
        int worstDi = -1, worstDj = -1;
        int candSetDiffBoards = 0;     // exact/honest 候选集不同的盘数
        int configCountDiff = 0;       // lib.totalPossibilities != ps.size() 的盘数
        int dupPrinted = 0;
        for (int it = 0; it < 300; ++it) {
            const int rows = 3, cols = 3;
            const int mines = 1 + g.rng.below(3);
            const ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
            const auto ps = ref::enumeratePlacements(rb);
            if (ps.empty() || ps.size() > 400) continue;
            const ObservedBoard b = toLibBoard(rb);
            const Basic::Result basic = Basic::Analyzer::analyze(b);
            Structure::ShapePool spool;
            const Structure::Result st = Structure::Analyzer::analyze(b, basic, spool);
            Distribution::DistPool dpool;  // 每盘新建：排除跨盘池缓存污染
            const Probability::Result prob = Exact::analyze(b, basic, st, dpool);
            const auto info = ref::aggregate(rb, ps);

            Grid<long double> exact(rows, cols, 0.0L), honest(rows, cols, 0.0L),
                half(rows, cols, 0.0L);
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    if (b.board[i][j] != Cell::Hidden) continue;
                    const long double pe = prob.mineProbability(b.id(i, j), b, basic, st);
                    const long double pa = info.total
                        ? static_cast<long double>(
                              info.mineCount[static_cast<std::size_t>(rb.flat(i, j))]) /
                              static_cast<long double>(info.total)
                        : 0.0L;
                    exact[i][j] = pe;
                    honest[i][j] = pa;
                    half[i][j] = 0.5L;
                    if (basic.marks[i][j] == Basic::Mark::Mine && pe != 1.0L)
                        ++basicMineBad;
                    if (pe > 1.0L) ++floatAbove1;
                    if (pe > 0.9999999L && pe < 1.0L) {
                        ++floatNear1;
                        minNear1 = (std::min)(minNear1, pe);
                    }
                    if (std::fabs(pe - pa) > 1e-9L) ++exactVsAggDiff;
                    if (std::fabs(pe - pa) > worstDiff) {
                        worstDiff = std::fabs(pe - pa);
                        worstDi = i;
                        worstDj = j;
                    }
                }

            const auto candEx = candCellsFrom(b, exact);
            const auto candHo = candCellsFrom(b, honest);
            if (candEx != candHo) ++candSetDiffBoards;

            auto check = [&](GridStat& s, const Grid<long double>& grid,
                             const std::vector<std::pair<int, int>>& cand,
                             bool dumpIfBad) {
                EndgameBruteforce::Config cfg;
                cfg.checkAllMoves = true;
                const EndgameBruteforce::Result lib =
                    EndgameBruteforce::solveEndgame(b, basic, st, dpool, grid, cfg);
                if (lib.totalPossibilities != static_cast<int>(ps.size())) ++configCountDiff;
                const auto naive = naivePerMove(rb, ps, cand);
                if (lib.result.size() != naive.size()) {
                    ++s.boardMismatch;
                    if (dumpIfBad && dupPrinted < 3) {
                        ++dupPrinted;
                        std::printf("\n--- exact-mismatch #%d: size %zu vs %zu ---\n",
                                    dupPrinted, lib.result.size(), naive.size());
                    }
                    return;
                }
                bool same = true;
                for (std::size_t j = 0; j < naive.size(); ++j)
                    if (lib.result[j].wins != naive[j]) { ++s.moveMismatch; same = false; }
                if (!same) ++s.boardMismatch;
                if (!same && dumpIfBad && dupPrinted < 3) {
                    ++dupPrinted;
                    std::printf("\n--- exact-grid mismatch board #%d ---\n", dupPrinted);
                    std::printf("grid:\n");
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::printf("%c ", b.board[i][j] == Cell::Hidden ? '.'
                                                    : static_cast<char>('0' + static_cast<int>(b.board[i][j])));
                        std::printf("\n");
                    }
                    std::printf("marks(S0 M1 F2 U3):\n");
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::printf("%d ", static_cast<int>(basic.marks[i][j]));
                        std::printf("\n");
                    }
                    std::printf("trueMines:");
                    for (int f : rb.trueMines) std::printf(" %d", f);
                    std::printf("\nper-cell (hidden)  mark |  exact p | honest p | diff\n");
                    for (int i = 1; i <= b.rows; ++i)
                        for (int j = 1; j <= b.cols; ++j) {
                            if (b.board[i][j] != Cell::Hidden) continue;
                            const long double pe =
                                prob.mineProbability(b.id(i, j), b, basic, st);
                            const long double pa = info.total
                                ? static_cast<long double>(info.mineCount[static_cast<std::size_t>(rb.flat(i, j))]) /
                                      static_cast<long double>(info.total)
                                : 0.0L;
                            std::printf("  (%d,%d) m=%d %.21Lf %.21Lf %.3g\n", i, j,
                                        static_cast<int>(basic.marks[i][j]), pe, pa,
                                        static_cast<double>(std::fabs(pe - pa)));
                        }
                    std::printf("cand(exact) :");
                    for (auto [x, y] : candEx) std::printf(" (%d,%d)", x, y);
                    std::printf("\ncand(honest):");
                    for (auto [x, y] : candHo) std::printf(" (%d,%d)", x, y);
                    std::printf("\nps=%zu lib.total=%d\n", ps.size(), lib.totalPossibilities);
                    std::printf("basic.mineSum=%d unknownSum=%d totalMines=%d  M=%d tSum=%d\n",
                                basic.mineSum, basic.unknownSum, b.totalMines,
                                b.totalMines - basic.mineSum, basic.unknownSum);
                    std::printf("prob.candidates(=denom)=%.6Lf  tCellProb=%.21Lf\n",
                                prob.candidates, prob.tCellProbability);
                    for (std::size_t c = 0; c < st.components.size(); ++c) {
                        const auto& inst = st.components[c];
                        std::printf("  comp%zu hash=%016llx%016llx boxes:", c,
                                    static_cast<unsigned long long>(inst.shape->hash.hi),
                                    static_cast<unsigned long long>(inst.shape->hash.lo));
                        for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
                            std::printf("[");
                            for (std::size_t k = inst.boxes.boxOf[bb];
                                 k < inst.boxes.boxOf[bb + 1]; ++k) {
                                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                                std::printf("(%d,%d)", x, y);
                            }
                            std::printf("]");
                        }
                        std::printf(" cons:");
                        for (std::size_t ci = 0; ci < inst.shape->constraints.size(); ++ci) {
                            const auto& lim = inst.shape->constraints[ci];
                            std::printf(" %d:[", lim.sum);
                            for (BoxId bid : lim.boxIds) std::printf("%d,", bid);
                            std::printf("]");
                        }
                        std::printf("\n    dist entries:");
                        const Distribution* d = Distribution::Solver::analyze(*inst.shape, dpool);
                        for (const auto& e : d->entries) {
                            std::printf(" (m=%d w=%.3Lf exp=[", e.mineCount, e.ways);
                            for (long double v : e.perBoxExpectation)
                                std::printf("%.4Lf,", v);
                            std::printf("])");
                        }
                        std::printf("\n    prob.components[%zu] boxProbs:", c);
                        if (c < prob.components.size())
                            for (long double v : prob.components[c].boxProbs)
                                std::printf(" %.6Lf", v);
                        std::printf("\n");
                    }
                    for (std::size_t j = 0; j < naive.size(); ++j)
                        std::printf("  move#%zu (%d,%d) lib w=%d naive w=%d %s\n", j,
                                    lib.result[j].x, lib.result[j].y, lib.result[j].wins,
                                    naive[j], lib.result[j].wins == naive[j] ? "" : "  <<<");
                    // 对分歧 move：逐方案真实数字 + 邻居分类
                    for (std::size_t j = 0; j < naive.size(); ++j) {
                        if (lib.result[j].wins == naive[j]) continue;
                        const int ci = static_cast<int>(j);
                        const int jx = lib.result[j].x, jy = lib.result[j].y;
                        std::printf("  digit table for move (%d,%d) [cand#%d]:\n", jx, jy, ci);
                        std::printf("    neighbors: ");
                        for (int di = -1; di <= 1; ++di)
                            for (int dj = -1; dj <= 1; ++dj) {
                                if (di == 0 && dj == 0) continue;
                                const int nx = jx + di, ny = jy + dj;
                                if (nx < 1 || nx > b.rows || ny < 1 || ny > b.cols) continue;
                                const bool isCand = std::find(candEx.begin(), candEx.end(),
                                                              std::make_pair(nx, ny)) != candEx.end();
                                const bool isMineM = basic.marks[nx][ny] == Basic::Mark::Mine;
                                std::printf("(%d,%d)%s%s ", nx, ny, isCand ? "C" : "-",
                                            isMineM ? "M" : "");
                            }
                        std::printf("\n    per config naive digit (j 自身是雷=-1):");
                        std::vector<int> digs(ps.size(), 0);
                        for (std::size_t ci2 = 0; ci2 < ps.size(); ++ci2) {
                            std::vector<char> mk(static_cast<std::size_t>(b.rows * b.cols), 0);
                            for (int f : ps[ci2]) mk[static_cast<std::size_t>(f)] = 1;
                            if (mk[static_cast<std::size_t>(rb.flat(jx, jy))]) {
                                digs[ci2] = -1;
                                continue;
                            }
                            int cnt = 0;
                            ref::forEa(jx, jy, b.rows, b.cols, [&](int ni, int nj) {
                                if (b.board[ni][nj] == Cell::Hidden &&
                                    mk[static_cast<std::size_t>(rb.flat(ni, nj))])
                                    ++cnt;
                            });
                            digs[ci2] = cnt;
                        }
                        for (int d : digs) std::printf(" %d", d);
                        std::printf("\n");
                    }
                }
            };
            check(stat[0], exact, candEx, true);
            check(stat[1], honest, candHo, false);
            check(stat[2], half, candCellsFrom(b, half), false);
            ++boards;
        }
        std::printf("boards=%d\n", boards);
        for (const auto& s : stat)
            std::printf("  [%s] board-mismatch=%d move-mismatch=%d\n", s.name,
                        s.boardMismatch, s.moveMismatch);
        std::printf("basic.Mine 格 getter != 1.0L: %d\n", basicMineBad);
        std::printf("boxProbs 0.9999999<v<1.0 出现次数: %d  最近 1.0(下方): %.21Lf\n",
                    floatNear1, minNear1);
        std::printf("boxProbs v>1.0 出现次数: %d\n", floatAbove1);
        std::printf("exact 与 aggregate 在隐藏格上 |diff|>1e-9 的格数: %d\n", exactVsAggDiff);
        std::printf("worst |exact-aggregate|: %.3g at (%d,%d)\n",
                    static_cast<double>(worstDiff), worstDi, worstDj);
        std::printf("exact/honest 候选集不同的盘数: %d\n", candSetDiffBoards);
        std::printf("lib.totalPossibilities != ps.size() 的盘数: %d\n", configCountDiff);
    }

    // ── Part 3：共享 DistPool + 逐盘新建 ShapePool（真实客户端模式）──
    // 验证假设：DistPool 以 Shape* 指针为键；ShapePool 跨盘新建时堆地址复用，
    // 共享 DistPool 命中「旧盘的分布」→ Exact::analyze 的 boxProbs 变成 NaN/值反转。
    std::printf("\n=== Part 3: shared DistPool, per-board ShapePool ===\n");
    {
        Gen g(0xDEADBEEF9876ULL);
        Distribution::DistPool sharedPool;
        std::map<const Structure::Shape*, U128> seenShape;  // 指针 → 内容哈希
        int boards = 0;
        int ptrReuseBad = 0;         // 同指针、不同内容（污染命中前提）
        int cacheMismatchComps = 0;  // 共享缓存 vs 新鲜池重算 逐位不等
        int nanCells = 0;            // 共享池下 exact 网格 NaN 格数
        int exactMismatchBoards = 0, exactMismatchMoves = 0;
        std::vector<std::pair<U128, U128>> reuseExamples;
        for (int it = 0; it < 300; ++it) {
            const int rows = 3, cols = 3;
            const int mines = 1 + g.rng.below(3);
            const ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
            const auto ps = ref::enumeratePlacements(rb);
            if (ps.empty() || ps.size() > 400) continue;
            const ObservedBoard b = toLibBoard(rb);
            const Basic::Result basic = Basic::Analyzer::analyze(b);
            Structure::ShapePool spool;  // 每盘新建 → 堆地址可能跨盘复用
            const Structure::Result st = Structure::Analyzer::analyze(b, basic, spool);
            const Probability::Result prob = Exact::analyze(b, basic, st, sharedPool);

            // 1) 指针跨盘复用检测 + 共享缓存逐位核对
            for (const auto& inst : st.components) {
                const Structure::Shape* p = inst.shape;
                const auto itF = seenShape.find(p);
                if (itF != seenShape.end() && !(itF->second == inst.shape->hash)) {
                    ++ptrReuseBad;
                    if (reuseExamples.size() < 3)
                        reuseExamples.push_back({itF->second, inst.shape->hash});
                }
                seenShape[p] = inst.shape->hash;
                const Distribution* dShared = Distribution::Solver::analyze(*p, sharedPool);
                Distribution::DistPool freshPool;
                const Distribution* dFresh = Distribution::Solver::analyze(*p, freshPool);
                bool eq = dShared->entries.size() == dFresh->entries.size();
                for (std::size_t e = 0; eq && e < dShared->entries.size(); ++e) {
                    const auto& a = dShared->entries[e];
                    const auto& b2 = dFresh->entries[e];
                    if (a.mineCount != b2.mineCount || a.ways != b2.ways ||
                        a.perBoxExpectation != b2.perBoxExpectation)
                        eq = false;
                }
                if (!eq) ++cacheMismatchComps;
            }

            // 2) NaN 计数
            for (int i = 1; i <= b.rows; ++i)
                for (int j = 1; j <= b.cols; ++j) {
                    if (b.board[i][j] != Cell::Hidden) continue;
                    const long double pe = prob.mineProbability(b.id(i, j), b, basic, st);
                    if (pe != pe) ++nanCells;
                }

            // 3) 污染下 exact 网格 lib vs naive
            Grid<long double> exact(rows, cols, 0.0L);
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j)
                    if (b.board[i][j] == Cell::Hidden)
                        exact[i][j] = prob.mineProbability(b.id(i, j), b, basic, st);
            const auto cand = candCellsFrom(b, exact);
            EndgameBruteforce::Config cfg;
            cfg.checkAllMoves = true;
            const EndgameBruteforce::Result lib =
                EndgameBruteforce::solveEndgame(b, basic, st, sharedPool, exact, cfg);
            const auto naive = naivePerMove(rb, ps, cand);
            bool same = lib.result.size() == naive.size();
            for (std::size_t j = 0; same && j < naive.size(); ++j)
                if (lib.result[j].wins != naive[j]) same = false;
            if (!same) {
                ++exactMismatchBoards;
                for (std::size_t j = 0; j < naive.size() && j < lib.result.size(); ++j)
                    if (lib.result[j].wins != naive[j]) ++exactMismatchMoves;
            }
            ++boards;
        }
        std::printf("boards=%d\n", boards);
        std::printf("指针跨盘复用(同指针、不同内容哈希) 组件数: %d\n", ptrReuseBad);
        for (const auto& [a, c] : reuseExamples)
            std::printf("  例: 旧哈希=%016llx%016llx -> 新哈希=%016llx%016llx\n",
                        static_cast<unsigned long long>(a.hi),
                        static_cast<unsigned long long>(a.lo),
                        static_cast<unsigned long long>(c.hi),
                        static_cast<unsigned long long>(c.lo));
        std::printf("共享缓存 vs 新鲜池重算 逐位不等 组件数: %d\n", cacheMismatchComps);
        std::printf("共享池下 exact 网格 NaN 格数: %d\n", nanCells);
        std::printf("共享池下 exact-grid lib vs naive: boards=%d moves=%d\n",
                    exactMismatchBoards, exactMismatchMoves);
    }
    return 0;
}