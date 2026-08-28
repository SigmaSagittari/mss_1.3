// harness.cpp — 差分测试主程序。
// 被测对象：mss/src 各层；参考实现：ref_mining.h（独立编写）。
// 原则：随机盘面（非手工定制）、固定种子、改进前后对比数据留档。
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/config.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"
#include "core/utility/radix_sort.h"
#include "core/utility/rng.h"

#include "ref_mining.h"

using namespace mss;

// ── 测试统计 ──
namespace T {
int checks = 0;
int fails = 0;
std::string cur;
void section(const char* name) { cur = name; }
void fail(const std::string& msg) {
    ++fails;
    if (fails <= 30) std::printf("  [FAIL] %s: %s\n", cur.c_str(), msg.c_str());
}
}  // namespace T

#define CHECK(cond, ...)                                        \
    do {                                                        \
        ++T::checks;                                            \
        if (!(cond)) {                                          \
            char buf[512];                                      \
            std::snprintf(buf, sizeof(buf), __VA_ARGS__);       \
            T::fail(std::string(buf));                          \
        }                                                       \
    } while (0)

static bool approx(long double a, long double b, long double tol = 1e-9L) {
    const long double d = std::fabs(a - b);
    const long double s = std::fabs(a) + std::fabs(b);
    return d <= tol * (s > 1.0L ? s : 1.0L);
}

static Cell toCell(int v) {
    if (v < 0) return Cell::Hidden;
    return static_cast<Cell>(v);
}

// 参考盘面 → 库盘面（并保留真实雷位以备 reveal 回放）。
static ObservedBoard toLibBoard(const ref::RefBoard& r) {
    ObservedBoard b(r.rows, r.cols, r.totalMines);
    for (int i = 1; i <= r.rows; ++i)
        for (int j = 1; j <= r.cols; ++j) b.board[i][j] = toCell(r.at(i, j));
    return b;
}

// ── 生成一致的随机盘面（真实雷位 → 数字）──
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
    // 数字格 + 额外隐藏（非雷格随机隐藏，产生 Unknown/T 格）
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

// ── 测试 1：Basic::Analyzer vs 参考标记 ──
static void testBasicAnalyzer(Gen& g, int iter) {
    T::section("T1 Basic::Analyzer vs reference");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3 + g.rng.below(3), cols = 3 + g.rng.below(3);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb;
        if (it % 3 != 0) {
            rb = genConsistent(g, rows, cols, mines, cols);  // 一致盘面为主
        } else {
            rb = ref::RefBoard(rows, cols, mines);
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j)
                    rb.at(i, j) = (g.rng.below(3) == 0) ? -1 : g.rng.below(9);  // 可能不一致
        }
        const ObservedBoard b = toLibBoard(rb);
        const Basic::Result res = Basic::Analyzer::analyze(b);

        // 参考标记
        std::vector<char> isMine(static_cast<std::size_t>(rows * cols), 0);
        std::vector<char> isSafe(static_cast<std::size_t>(rows * cols), 0);
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j)
                if (rb.at(i, j) >= 0) isSafe[static_cast<std::size_t>(rb.flat(i, j))] = 1;
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j) {
                if (rb.at(i, j) != -1) continue;
                bool forced = false;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    const int v = rb.at(ni, nj);
                    if (v < 0) return;
                    int hc = 0;
                    ref::forEa(ni, nj, rows, cols, [&](int ai, int aj) {
                        if (rb.at(ai, aj) < 0) ++hc;
                    });
                    if (hc == v) forced = true;
                });
                if (forced) isMine[static_cast<std::size_t>(rb.flat(i, j))] = 1;
            }
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j) {
                if (rb.at(i, j) != -1) continue;
                bool safe = false;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    const int v = rb.at(ni, nj);
                    if (v < 0) return;
                    int mc = 0;
                    ref::forEa(ni, nj, rows, cols, [&](int ai, int aj) {
                        if (rb.at(ai, aj) < 0 && isMine[static_cast<std::size_t>(rb.flat(ai, aj))]) ++mc;
                    });
                    if (mc == v) safe = true;
                });
                if (safe) isSafe[static_cast<std::size_t>(rb.flat(i, j))] = 1;
            }
        // 反解不一致盘面时，一致性校验
        bool refValid = true;
        for (int i = 1; i <= rows && refValid; ++i)
            for (int j = 1; j <= cols; ++j) {
                const int v = rb.at(i, j);
                if (v < 0) continue;
                int mc = 0, hc = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (rb.at(ni, nj) < 0) ++hc;
                    if (isMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++mc;
                });
                if (v < mc || v > mc + hc) refValid = false;
            }
        CHECK(res.valid == refValid, "valid flag mismatch it=%d", it);
        if (res.valid) {
            int um = 0, mm = 0;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const Basic::Mark m = res.marks[i][j];
                    const int f = rb.flat(i, j);
                    if (rb.at(i, j) < 0) {
                        if (isMine[static_cast<std::size_t>(f)] &&
                            m != Basic::Mark::Mine)
                            CHECK(false, "expected Mine at (%d,%d) it=%d", i, j, it);
                        if (!isMine[static_cast<std::size_t>(f)] && !isSafe[static_cast<std::size_t>(f)] &&
                            m == Basic::Mark::Mine)
                            CHECK(false, "unexpected Mine at (%d,%d) it=%d", i, j, it);
                        if (isSafe[static_cast<std::size_t>(f)] && m == Basic::Mark::Unknown)
                            CHECK(false, "expected Safe/Frontier at (%d,%d) it=%d", i, j, it);
                    } else if (m != Basic::Mark::Safe) {
                        CHECK(false, "revealed cell not Safe at (%d,%d) it=%d", i, j, it);
                    }
                    if (m == Basic::Mark::Unknown) ++um;
                    if (m == Basic::Mark::Mine) ++mm;
                }
            CHECK(um == res.unknownSum, "unknownSum=%d vs %d it=%d", res.unknownSum, um, it);
            CHECK(mm == res.mineSum, "mineSum=%d vs %d it=%d", res.mineSum, mm, it);
        }
    }
}

// ── 测试 2：Basic::Updater 增量 vs 全量重析（语义剪枝，校验不变量 + 重放）──
static bool g_t2Dumped = false;

static void dumpT2(const ref::RefBoard& rb, const ObservedBoard& b,
                   const Basic::Result& res,
                   const std::vector<Basic::Update>& allUpdates) {
    FILE* f = std::fopen("dump_t2.txt", "w");
    if (!f) return;
    std::fprintf(f, "rows=%d cols=%d totalMines=%d\n", b.rows, b.cols, b.totalMines);
    std::fprintf(f, "board:\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j) {
            const Cell c = b.board[i][j];
            std::fprintf(f, "%s ", c == Cell::Hidden ? "." : std::to_string(static_cast<int>(c)).c_str());
        }
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "trueMines:");
    for (int fz : rb.trueMines) std::fprintf(f, " %d", fz);
    std::fprintf(f, "\nupdates(%zu):\n", allUpdates.size());
    for (const auto& u : allUpdates) {
        const auto [x, y] = b.pos(u.cell);
        std::fprintf(f, "  cell(%d,%d) -> %s\n", x, y,
                     u.next == Cell::Hidden ? "Hidden"
                                            : std::to_string(static_cast<int>(u.next)).c_str());
    }
    std::fprintf(f, "marks:\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(res.marks[i][j]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "sums: u=%d m=%d valid=%d\n", res.unknownSum, res.mineSum,
                 static_cast<int>(res.valid));
    std::fclose(f);
}

static void testBasicUpdater(Gen& g, int iter) {
    T::section("T2 Basic::Updater invariants");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3 + g.rng.below(3), cols = 3 + g.rng.below(3);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        std::vector<char> trueMine(static_cast<std::size_t>(rows * cols), 0);
        for (int f : rb.trueMines) trueMine[static_cast<std::size_t>(f)] = 1;

        ObservedBoard b = toLibBoard(rb);
        Basic::Result res = Basic::Analyzer::analyze(b);
        // root snapshot 用于重放
        const Basic::Result root = res;
        const ObservedBoard rootBoard = b;

        const int steps = 1 + g.rng.below(6);
        std::vector<Basic::Delta> deltas;
        std::vector<Basic::Update> allUpdates;
        for (int s = 0; s < steps; ++s) {
            const int failsBefore = T::fails;
            // 随机选：reveal 一个非雷隐藏格，或回滚一个已揭示数字格
            // （雷格不可翻开：翻开即死，不是合法盘面演化）
            std::vector<std::pair<int, int>> hidden, shown;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const bool isMineCell =
                        trueMine[static_cast<std::size_t>(rb.flat(i, j))];
                    if (b.board[i][j] == Cell::Hidden && !isMineCell)
                        hidden.emplace_back(i, j);
                    else if (b.board[i][j] != Cell::Hidden)
                        shown.emplace_back(i, j);
                }
            std::vector<Basic::Update> ups;
            if (!hidden.empty() && (shown.empty() || g.rng.below(2) == 0)) {
                const auto [i, j] = hidden[g.rng.below(static_cast<int>(hidden.size()))];
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (trueMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++v;
                });
                b.board[i][j] = toCell(v);
                ups.push_back({b.id(i, j), b.board[i][j]});
            } else {
                const auto [i, j] = shown[g.rng.below(static_cast<int>(shown.size()))];
                b.board[i][j] = Cell::Hidden;
                ups.push_back({b.id(i, j), Cell::Hidden});
            }
            Basic::Delta d = Basic::Updater::update(b, res, ups);
            deltas.push_back(d);
            allUpdates.insert(allUpdates.end(), ups.begin(), ups.end());

            // 不变量
            int um = 0, mm = 0;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const Basic::Mark m = res.marks[i][j];
                    if (m == Basic::Mark::Unknown) ++um;
                    if (m == Basic::Mark::Mine) ++mm;
                }
            CHECK(um == res.unknownSum, "unknownSum drift step%d it=%d", s, it);
            CHECK(mm == res.mineSum, "mineSum drift step%d it=%d", s, it);
            // Mine 支持：每个 Mine 必被某个饱和数字（hiddenCount==v）邻接
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    if (res.marks[i][j] != Basic::Mark::Mine) continue;
                    bool sup = false;
                    ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                        const int v = static_cast<int>(b.board[ni][nj]);
                        if (!(v >= 0 && v <= 8)) return;
                        int hc = 0;
                        ref::forEa(ni, nj, rows, cols, [&](int ai, int aj) {
                            if (b.board[ai][aj] == Cell::Hidden) ++hc;
                        });
                        if (hc == v) sup = true;
                    });
                    CHECK(sup, "unsupported Mine at (%d,%d) step%d it=%d", i, j, s, it);
                }
            // Frontier 必有邻接数字；Unknown 必无邻接数字（board 当前态）
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    bool hasNum = false;
                    ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                        const int v = static_cast<int>(b.board[ni][nj]);
                        if (v >= 0 && v <= 8) hasNum = true;
                    });
                    if (res.marks[i][j] == Basic::Mark::Frontier)
                        CHECK(hasNum, "Frontier w/o number (%d,%d) step%d it=%d", i, j, s, it);
                    if (res.marks[i][j] == Basic::Mark::Unknown)
                        CHECK(!hasNum, "Unknown with number (%d,%d) step%d it=%d", i, j, s, it);
                }
            // 每数字格一致性（除非盘面矛盾）
            if (res.valid) {
                for (int i = 1; i <= rows; ++i)
                    for (int j = 1; j <= cols; ++j) {
                        const int v = static_cast<int>(b.board[i][j]);
                        if (!(v >= 0 && v <= 8)) continue;
                        int mc = 0, hc = 0;
                        ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                            if (b.board[ni][nj] == Cell::Hidden) ++hc;
                            if (res.marks[ni][nj] == Basic::Mark::Mine) ++mc;
                        });
                        CHECK(v >= mc && v <= mc + hc,
                              "number (%d,%d)=%d mc=%d hc=%d step%d it=%d", i, j, v, mc, hc, s, it);
                    }
            }
            if (T::fails > failsBefore && !g_t2Dumped) {
                g_t2Dumped = true;
                dumpT2(rb, b, res, allUpdates);
                return;
            }
        }  // steps

        // Delta 重放：从 root 起逐条 apply 应等于最终增量结果
        Basic::Result replay = root;
        for (const auto& d : deltas) Basic::Updater::applyDelta(replay, d);
        bool same = replay.rows == res.rows && replay.cols == res.cols &&
                    replay.unknownSum == res.unknownSum && replay.mineSum == res.mineSum;
        int diff = 0;
        for (int i = 1; i <= rows && same; ++i)
            for (int j = 1; j <= cols; ++j)
                if (replay.marks[i][j] != res.marks[i][j]) { same = false; ++diff; }
        CHECK(same, "delta replay mismatch it=%d", it);
    }
}

// ── 测试 3：Structure::Updater::update + applyDelta == 全量重析 ──
static void dumpT3Call(int id, const char* tag, const ObservedBoard& b,
                       const Basic::Result& basic, const Structure::Result& r,
                       const std::vector<Basic::Update>& ups) {
    char path[64];
    std::snprintf(path, sizeof(path), "dumpT3/%03d_%s.txt", id, tag);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols, b.totalMines);
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j) {
            const Cell c = b.board[i][j];
            std::fprintf(f, "%s ", c == Cell::Hidden ? "."
                                                      : std::to_string(static_cast<int>(c)).c_str());
        }
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "basic marks (S=0 M=1 F=2 U=3):\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "updates(%zu):\n", ups.size());
    for (const auto& u : ups) {
        const auto [x, y] = b.pos(u.cell);
        std::fprintf(f, "  (%d,%d)->%s\n", x, y,
                     u.next == Cell::Hidden ? "H" : std::to_string(static_cast<int>(u.next)).c_str());
    }
    std::fprintf(f, "components=%zu\n", r.components.size());
    for (std::size_t c = 0; c < r.components.size(); ++c) {
        const auto& inst = r.components[c];
        std::fprintf(f, "  comp%zu hash=%016llx%016llx boxes=", c,
                     static_cast<unsigned long long>(inst.shape->hash.hi),
                     static_cast<unsigned long long>(inst.shape->hash.lo));
        for (std::size_t b2 = 0; b2 < inst.boxes.count(); ++b2) {
            std::fprintf(f, "[");
            for (std::size_t k = inst.boxes.boxOf[b2]; k < inst.boxes.boxOf[b2 + 1]; ++k) {
                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                std::fprintf(f, "(%d,%d)", x, y);
            }
            std::fprintf(f, "]");
        }
        std::fprintf(f, " cons=");
        for (CellId c2 : inst.constraintCells) {
            const auto [x, y] = b.pos(c2);
            std::fprintf(f, "(%d,%d)", x, y);
        }
        std::fprintf(f, "\n");
    }
    std::fclose(f);
}

static void dumpT3Delta(int id, const ObservedBoard& b, const Structure::Delta& d) {
    char path[64];
    std::snprintf(path, sizeof(path), "dumpT3/%03d_delta.txt", id);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "removed:");
    for (int c : d.removed) std::fprintf(f, " %d", c);
    std::fprintf(f, "\nadded:%zu\n", d.added.size());
    for (std::size_t i = 0; i < d.added.size(); ++i)
        std::fprintf(f, "  add id=%d (data %zu)\n", d.added[i], d.addedData.size());
    std::fclose(f);
}

static void testStructureUpdate(Gen& g, int iter) {
    T::section("T3 Structure update == analyze");
    // 记录第二个盘面用于重放链路
    for (int it = 0; it < iter; ++it) {
        std::printf("T3 it=%d\n", it);
        const int rows = 3 + g.rng.below(3), cols = 3 + g.rng.below(3);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        std::vector<char> trueMine(static_cast<std::size_t>(rows * cols), 0);
        for (int f : rb.trueMines) trueMine[static_cast<std::size_t>(f)] = 1;

        ObservedBoard b = toLibBoard(rb);
        Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool pool;
        Structure::Result root = Structure::Analyzer::analyze(b, basic, pool);
        {
            // 幂等性检查：相同输入二次全量 analyze 必须一致（否则线程局部队列状态被污染）
            Structure::Result root2 = Structure::Analyzer::analyze(b, basic, pool);
            bool idem = root.components.size() == root2.components.size();
            for (std::size_t c = 0; c < root.components.size() && idem; ++c)
                if (root.components[c].shape->hash != root2.components[c].shape->hash ||
                    root.components[c].boxes.cells != root2.components[c].boxes.cells ||
                    root.components[c].constraintCells != root2.components[c].constraintCells)
                    idem = false;
            if (!idem) {
                CHECK(false, "analyze NOT idempotent it=%d (comps %zu vs %zu)", it,
                      root.components.size(), root2.components.size());
            }
            {
                char path[64];
                std::snprintf(path, sizeof(path), "dumpT3/anal_%04d.txt", it);
                FILE* f = std::fopen(path, "w");
                if (f) {
                    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols,
                                 b.totalMines);
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::fprintf(f, "%s ",
                                         b.board[i][j] == Cell::Hidden
                                             ? "."
                                             : std::to_string(static_cast<int>(b.board[i][j])).c_str());
                        std::fprintf(f, "\n");
                    }
                    std::fprintf(f, "marks:\n");
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
                        std::fprintf(f, "\n");
                    }
                    std::fprintf(f, "trueMines:");
                    for (int fz : rb.trueMines) std::fprintf(f, " %d", fz);
                    std::fprintf(f, "\ncomps=%zu\n", root.components.size());
                    for (std::size_t c = 0; c < root.components.size(); ++c) {
                        const auto& inst = root.components[c];
                        std::fprintf(f, " c%zu h=%016llx%016llx\n", c,
                                     static_cast<unsigned long long>(inst.shape->hash.hi),
                                     static_cast<unsigned long long>(inst.shape->hash.lo));
                    }
                    std::fclose(f);
                }
            }
        }
        const Basic::Result rootBasic = basic;
        const ObservedBoard rootBoard = b;

        std::vector<Structure::Delta> deltas;
        const int steps = 1 + g.rng.below(5);
        for (int s = 0; s < steps; ++s) {
            std::vector<std::pair<int, int>> hidden, shown;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const bool isMineCell =
                        trueMine[static_cast<std::size_t>(rb.flat(i, j))];
                    if (b.board[i][j] == Cell::Hidden && !isMineCell)
                        hidden.emplace_back(i, j);
                    else if (b.board[i][j] != Cell::Hidden)
                        shown.emplace_back(i, j);
                }
            std::vector<Basic::Update> ups;
            if (!hidden.empty() && (shown.empty() || g.rng.below(2) == 0)) {
                const auto [i, j] = hidden[g.rng.below(static_cast<int>(hidden.size()))];
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (trueMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++v;
                });
                b.board[i][j] = toCell(v);
                ups.push_back({b.id(i, j), b.board[i][j]});
            } else {
                const auto [i, j] = shown[g.rng.below(static_cast<int>(shown.size()))];
                b.board[i][j] = Cell::Hidden;
                ups.push_back({b.id(i, j), Cell::Hidden});
            }
            Basic::Result nextBasic = Basic::Analyzer::analyze(b);  // 全量基准
            dumpT3Call(1000 * it + s, "pre_update", b, nextBasic, root, ups);
            Structure::Delta d =
                Structure::Updater::update(b, nextBasic, root, pool, ups);
            dumpT3Delta(1000 * it + s, b, d);
            // 注意：update 就地修改 root —— root 现在是增量后的状态
            deltas.push_back(d);

            const Structure::Result full = Structure::Analyzer::analyze(b, nextBasic, pool);
            // 顺序无关比较：组件按 (hash, cells, boxOf, constraintCells) 对齐成多重集合，
            // 再按对齐关系核对 cellLoc（component 身份与 box 编号）。
            using CompKey = std::tuple<U128, std::vector<CellId>, std::vector<std::uint16_t>,
                                       std::vector<CellId>>;
            auto compKey = [](const Structure::Instance& c) {
                return CompKey(c.shape->hash, c.boxes.cells, c.boxes.boxOf,
                               c.constraintCells);
            };
            std::map<CompKey, int> setA, setB;
            std::vector<CompKey> keyListA, keyListB;
            for (const auto& c : root.components) {
                setA[compKey(c)]++;
                keyListA.push_back(compKey(c));
            }
            for (const auto& c : full.components) {
                setB[compKey(c)]++;
                keyListB.push_back(compKey(c));
            }
            bool same = (setA == setB);
            if (same) {
                auto idxOf = [](const std::vector<CompKey>& keys,
                                const CompKey& k) -> int {
                    for (int i = 0; i < static_cast<int>(keys.size()); ++i)
                        if (keys[static_cast<std::size_t>(i)] == k) return i;
                    return -1;
                };
                const std::size_t cellN = static_cast<std::size_t>((rows + 1) * (cols + 1));
                for (std::size_t k = 0; k < cellN; ++k) {
                    const CellLocation& la = root.cellLoc[k];
                    const CellLocation& lb = full.cellLoc[k];
                    if (la.component == -1 || lb.component == -1) {
                        if (la.component != lb.component) { same = false; break; }
                        continue;
                    }
                    const int ia = idxOf(keyListA, keyListA[static_cast<std::size_t>(la.component)]);
                    const int ib = idxOf(keyListB, keyListB[static_cast<std::size_t>(lb.component)]);
                    const bool sameComp = (ia == ib &&
                                           keyListA[static_cast<std::size_t>(ia)] ==
                                               keyListB[static_cast<std::size_t>(ib)]);
                    if (!sameComp || la.box != lb.box) { same = false; break; }
                }
            }
            CHECK(same, "structure mismatch after step %d it=%d (comps %zu vs %zu)",
                  s, it, root.components.size(), full.components.size());
        }

        // Delta 重放：fresh root → applyDelta 链 == 最终增量状态
        Structure::Result rp = Structure::Analyzer::analyze(rootBoard, rootBasic, pool);
        for (const auto& d : deltas) Structure::Updater::applyDelta(rp, d);
        bool same = rp.components.size() == root.components.size();
        for (int c = 0; c < static_cast<int>(root.components.size()) && same; ++c) {
            const auto& A = rp.components[static_cast<std::size_t>(c)];
            const auto& B = root.components[static_cast<std::size_t>(c)];
            if (A.shape->hash != B.shape->hash) { same = false; break; }
            if (A.constraintCells != B.constraintCells) { same = false; break; }
            if (A.boxes.cells != B.boxes.cells) { same = false; break; }
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>((rows + 1) * (cols + 1)) && same; ++k)
            if (rp.cellLoc[k].component != root.cellLoc[k].component ||
                rp.cellLoc[k].box != root.cellLoc[k].box)
                same = false;
        CHECK(same, "delta replay mismatch it=%d", it);
    }
}

// ── 测试 4：Exact::analyze vs 参考枚举 ──
static bool g_t4Dumped = false;

static void dumpT4(const ref::RefBoard& rb, const ObservedBoard& b,
                   const Basic::Result& basic, const Structure::Result& st,
                   const Probability::Result& prob,
                   const std::vector<std::vector<int>>& ps) {
    FILE* f = std::fopen("dump_t4.txt", "w");
    if (!f) return;
    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols, b.totalMines);
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%s ",
                         b.board[i][j] == Cell::Hidden
                             ? "."
                             : std::to_string(static_cast<int>(b.board[i][j])).c_str());
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "trueMines:");
    for (int fz : rb.trueMines) std::fprintf(f, " %d", fz);
    std::fprintf(f, "\nmarks:\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "ref placements=%zu:\n", ps.size());
    for (const auto& S : ps) {
        std::fprintf(f, "  {");
        for (int fz : S) std::fprintf(f, "%d,", fz);
        std::fprintf(f, "}\n");
    }
    std::fprintf(f, "lib components=%zu:\n", st.components.size());
    for (std::size_t c = 0; c < st.components.size(); ++c) {
        const auto& inst = st.components[c];
        std::fprintf(f, "  c%zu boxes:", c);
        for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
            std::fprintf(f, "[");
            for (std::size_t k = inst.boxes.boxOf[bb]; k < inst.boxes.boxOf[bb + 1]; ++k) {
                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                std::fprintf(f, "(%d,%d)", x, y);
            }
            std::fprintf(f, "]");
        }
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "lib candidates=%Lf\n", prob.candidates);
    for (std::size_t c = 0; c < st.components.size(); ++c) {
        const auto& inst = st.components[c];
        std::fprintf(f, "  c%zu boxProbs:", c);
        for (long double p : prob.components[static_cast<std::size_t>(c)].boxProbs)
            std::fprintf(f, " %Lf", p);
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "tCell=%Lf\n", prob.tCellProbability);
    std::fclose(f);
    g_t4Dumped = true;
}

static void testExactAnalyze(Gen& g, int iter) {
    T::section("T4 Exact::analyze vs enumeration");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3 + g.rng.below(2), cols = 3 + g.rng.below(2);
        const int mines = 1 + g.rng.below(std::min(rows * cols / 2, 3));
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        // 限制隐藏格数，否则枚举爆炸
        const auto hidden = rb.hiddenCells();
        if (static_cast<int>(hidden.size()) > 12) continue;
        auto ps = ref::enumeratePlacements(rb);
        if (ps.empty()) continue;
        const auto info = ref::aggregate(rb, ps);

        const ObservedBoard b = toLibBoard(rb);
        const Basic::Result basic = Basic::Analyzer::analyze(b);
        CHECK(basic.valid, "consistent board analyzed invalid it=%d", it);
        Structure::ShapePool pool;
        const Structure::Result st = Structure::Analyzer::analyze(b, basic, pool);
        Distribution::DistPool dpool;
        const Probability::Result prob = Exact::analyze(b, basic, st, dpool);

        // candidates == 方案数
        CHECK(approx(prob.candidates, static_cast<long double>(ps.size()), 1e-12L),
              "candidates %Lf vs %zu it=%d", prob.candidates, ps.size(), it);
        if (!approx(prob.candidates, static_cast<long double>(ps.size()), 1e-12L) &&
            !g_t4Dumped) {
            dumpT4(rb, b, basic, st, prob, ps);
            return;
        }

        // 每隐藏格概率
        for (const auto [i, j] : hidden) {
            const long double refP =
                info.total ? static_cast<long double>(info.mineCount[static_cast<std::size_t>(
                                rb.flat(i, j))]) /
                                 static_cast<long double>(info.total)
                           : 0.0L;
            const long double libP =
                prob.mineProbability(b.id(i, j), b, basic, st);
            if (!approx(refP, libP, 5e-8L)) {
                CHECK(false, "P(%d,%d) ref=%Lf lib=%Lf it=%d", i, j, refP, libP, it);
                break;
            }
        }
    }
}

// ── 测试 5：Exact::observe vs 参考枚举 ──
static void testExactObserve(Gen& g, int iter) {
    T::section("T5 Exact::observe vs enumeration");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3, cols = 3;
        const int mines = 1 + g.rng.below(3);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        const auto hidden = rb.hiddenCells();
        if (hidden.empty()) continue;
        auto ps = ref::enumeratePlacements(rb);
        if (ps.empty()) continue;

        const ObservedBoard b = toLibBoard(rb);
        const Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool pool;
        const Structure::Result st = Structure::Analyzer::analyze(b, basic, pool);
        Distribution::DistPool dpool;
        const Probability::Result prob = Exact::analyze(b, basic, st, dpool);

        // 只测前几个隐藏格（3x3 ≤9 隐藏，枚举 2^9 小）
        int tested = 0;
        for (const auto [i, j] : hidden) {
            if (tested++ >= 4) break;
            const auto digits = ref::observeDigits(rb, ps, i, j);
            const std::size_t tot = ps.size();
            const Probability::ObserveResult o =
                Exact::observe(b, basic, st, prob, dpool, b.id(i, j));
            long double refExpl = 0;
            {
                std::size_t cnt = 0;
                for (const auto& S : ps)
                    for (int f : S)
                        if (f == rb.flat(i, j)) { ++cnt; break; }
                refExpl = static_cast<long double>(cnt) / static_cast<long double>(tot);
            }
            CHECK(approx(o.explosion, refExpl, 5e-8L),
                  "explosion(%d,%d) ref=%Lf lib=%Lf it=%d", i, j, refExpl, o.explosion, it);
            long double sumP = 0;
            for (int k = 0; k < 9; ++k) {
                const long double refD =
                    static_cast<long double>(digits[static_cast<std::size_t>(k)]) /
                    static_cast<long double>(tot);
                CHECK(approx(o.digit[static_cast<std::size_t>(k)], refD, 5e-8L),
                      "digit[%d](%d,%d) ref=%Lf lib=%Lf it=%d", k, i, j, refD,
                      o.digit[static_cast<std::size_t>(k)], it);
                sumP += o.digit[static_cast<std::size_t>(k)];
            }
            CHECK(approx(sumP + o.explosion, 1.0L, 1e-7L),
                  "digit+explosion != 1 at (%d,%d) it=%d", i, j, it);
        }
    }
}

// ── 测试 6：EndgameBruteforce vs 朴素最优解（候选=全部隐藏格，真实 digit）──
// 独立朴素求解器：与库完全不同的递归（无剪枝无缓存），但同一博弈模型。
struct NaiveSolver {
    // 每个 placement 的每个候选格数字（真实邻雷数）
    std::vector<std::vector<int>> reveal;
    int m = 0;
    std::uint64_t calls = 0;

    int value(const std::vector<int>& configs, std::vector<char>& opened) {
        ++calls;
        if (configs.empty()) return 0;
        if (configs.size() == 1) return 1;  // 知道唯一配置即必胜
        // 安全格：先全部点开（与库同模型）
        std::vector<int> deadCnt(m, 0);
        for (int ci : configs)
            for (int j = 0; j < m; ++j)
                if (revealAt(ci, j) < 0) ++deadCnt[j];  // -1 标记：该格本身是雷
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
            if (deadCnt[j] == static_cast<int>(configs.size())) continue;  // 点谁死谁
            std::map<int, std::vector<int>> groups;
            for (int ci : configs) {
                if (revealAt(ci, j) < 0) continue;  // 该配置下 j 是雷 → 死
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

    // revealAt<0 表示该配置下这格是雷；否则为真实数字
    int revealAt(int ci, int j) const {
        return reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)];
    }
};

static int libBest(const EndgameBruteforce::Result& r) {
    int b = 0;
    for (const auto& w : r.result) b = (std::max)(b, w.wins);
    return b;
}

static void dumpT6(const ref::RefBoard& rb, const ObservedBoard& b,
                   const Basic::Result& basic,
                   const std::vector<std::vector<int>>& ps,
                   const EndgameBruteforce::Result& lib,
                   const std::vector<int>& naiveWins, int m) {
    FILE* f = std::fopen("dump_t6.txt", "w");
    if (!f) return;
    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols, b.totalMines);
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%s ",
                         b.board[i][j] == Cell::Hidden
                             ? "."
                             : std::to_string(static_cast<int>(b.board[i][j])).c_str());
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "trueMines:");
    for (int fz : rb.trueMines) std::fprintf(f, " %d", fz);
    std::fprintf(f, "\nmarks:\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "placements(%zu):\n", ps.size());
    for (const auto& S : ps) {
        std::fprintf(f, "  {");
        for (int fz : S) std::fprintf(f, "%d,", fz);
        std::fprintf(f, "}\n");
    }
    std::fprintf(f, "lib total=%d moves=%zu\n", lib.totalPossibilities, lib.result.size());
    for (int j = 0; j < m; ++j) {
        const char* cel = "";
        if (j < static_cast<int>(lib.result.size()))
            std::fprintf(f, "  move#%d lib(%d,%d,w=%d) naive w=%d\n", j,
                         lib.result[static_cast<std::size_t>(j)].x,
                         lib.result[static_cast<std::size_t>(j)].y,
                         lib.result[static_cast<std::size_t>(j)].wins, naiveWins[j]);
        else
            std::fprintf(f, "  move#%d naive w=%d (lib missing) %s\n", j, naiveWins[j], cel);
    }
    std::fclose(f);
}

static bool g_t6Dumped = false;

static void testEndgame(Gen& g, int iter, bool honestProbGrid) {
    T::section(honestProbGrid ? "T8 Endgame honest-prob-grid (P==1 exclusion)"
                              : "T6 Endgame naive vs library");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3, cols = 3;
        const int mines = 1 + g.rng.below(3);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        const auto hidden = rb.hiddenCells();
        if (hidden.empty()) continue;
        auto ps = ref::enumeratePlacements(rb);
        if (ps.empty() || ps.size() > 400) continue;  // 库 all_distribute 上限由 kMax 限制；这里避免长跑
        // 库枚举（mineConfigs）应与我方枚举一致 — 前置一致性
        const ObservedBoard b = toLibBoard(rb);
        const Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool pool;
        const Structure::Result st = Structure::Analyzer::analyze(b, basic, pool);
        Distribution::DistPool dpool;
        const auto info = ref::aggregate(rb, ps);

        // 概率网格：honestProbGrid → 真实 per-cell；否则全 0.5（全部隐藏格作候选）
        Grid<long double> probGrid(rows, cols, 0.0L);
        std::vector<char> isCandidate(static_cast<std::size_t>(rows * cols), 0);
        for (const auto [i, j] : hidden) {
            const long double p = info.total
                ? static_cast<long double>(info.mineCount[static_cast<std::size_t>(rb.flat(i, j))]) /
                      static_cast<long double>(info.total)
                : 0.0L;
            probGrid[i][j] = honestProbGrid ? p : 0.5L;
            const bool cand = !honestProbGrid || p < 1.0L;
            isCandidate[static_cast<std::size_t>(rb.flat(i, j))] = cand ? 1 : 0;
        }

        // 库求解
        EndgameBruteforce::Config cfg;
        cfg.checkAllMoves = true;
        EndgameBruteforce::Result lib =
            EndgameBruteforce::solveEndgame(b, basic, st, dpool, probGrid, cfg);
        const int nCand = lib.result.size();
        CHECK(static_cast<int>(ps.size()) == lib.totalPossibilities,
              "config-space mismatch: naive=%zu lib=%d it=%d", ps.size(),
              lib.totalPossibilities, it);

        // 朴素求解器（候选 = 与库相同的集合；真实 digit 含非候选固定雷）
        std::vector<std::vector<int>> candCells;   // 候选格 flat
        for (const auto [i, j] : hidden)
            if (isCandidate[static_cast<std::size_t>(rb.flat(i, j))])
                candCells.push_back({i, j});
        if (candCells.empty()) continue;
        NaiveSolver nv;
        nv.m = static_cast<int>(candCells.size());
        nv.reveal.assign(ps.size(), std::vector<int>(static_cast<std::size_t>(nv.m), 0));
        for (std::size_t ci = 0; ci < ps.size(); ++ci) {
            std::vector<char> mk(static_cast<std::size_t>(rows * cols), 0);
            for (int f : ps[ci]) mk[static_cast<std::size_t>(f)] = 1;
            for (int j = 0; j < nv.m; ++j) {
                const int i = candCells[static_cast<std::size_t>(j)][0];
                const int k = candCells[static_cast<std::size_t>(j)][1];
                if (mk[static_cast<std::size_t>(rb.flat(i, k))]) {
                    nv.reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)] = -1;
                    continue;
                }
                int cnt = 0;
                ref::forEa(i, k, rows, cols, [&](int ni, int nj) {
                    if (rb.at(ni, nj) < 0 &&
                        mk[static_cast<std::size_t>(rb.flat(ni, nj))])
                        ++cnt;
                });
                nv.reveal[static_cast<std::size_t>(ci)][static_cast<std::size_t>(j)] = cnt;
            }
        }
        std::vector<int> allC(static_cast<std::size_t>(ps.size()));
        for (int i = 0; i < static_cast<int>(allC.size()); ++i) allC[i] = i;
        std::vector<char> opened(static_cast<std::size_t>(nv.m), 0);
        // 逐招对比：朴素也要每个候选格的独立胜数 → 单独求解每个首招
        int naiveBest = 0;
        std::vector<int> naiveWins(static_cast<std::size_t>(nv.m), 0);
        for (int j = 0; j < nv.m; ++j) {  // 模拟 checkAllMoves：点 j，按观测分组后求和
            std::map<int, std::vector<int>> groups;
            for (int ci : allC)
                if (nv.revealAt(ci, j) >= 0) groups[nv.revealAt(ci, j)].push_back(ci);
            opened[static_cast<std::size_t>(j)] = 1;
            int w = 0;
            for (auto& [k, grp] : groups) w += nv.value(grp, opened);
            opened[static_cast<std::size_t>(j)] = 0;
            naiveWins[static_cast<std::size_t>(j)] = w;
            naiveBest = (std::max)(naiveBest, w);
        }
        CHECK(static_cast<int>(lib.result.size()) == nv.m,
              "candidate count lib=%zu naive=%d it=%d", lib.result.size(), nv.m, it);
        // 库 result 顺序 = 候选顺序（j 升序）；库的 j 序与 candCells 序一致吗？
        // buildCandidates 按 (i,j) 扫行 → 与 hiddenCells() 顺序一致（行序），
        // candCells 也是行序 → 对齐。
        bool sameWins = true;
        for (int j = 0; j < nv.m; ++j) {
            if (nCand != nv.m) break;
            const int libW = lib.result[static_cast<std::size_t>(j)].wins;
            const int naiW = naiveWins[static_cast<std::size_t>(j)];
            if (libW != naiW) {
                sameWins = false;
                if (T::fails < 30)
                    std::printf("  [FAIL-%s] move#%d lib=%d naive=%d it=%d (best lib=%d naive=%d)\n",
                                honestProbGrid ? "T8" : "T6", j, libW, naiW,
                                it, libBest(lib), naiveBest);
            }
        }
        CHECK(sameWins, "per-move win mismatch it=%d", it);
        if (!sameWins && !honestProbGrid && !g_t6Dumped) {
            g_t6Dumped = true;
            dumpT6(rb, b, basic, ps, lib, naiveWins, nv.m);
        }
    }
}

// ── 测试 7：radix_sort 正确性与稳定性 ──
static void testRadixSort(Gen& g, int iter) {
    T::section("T7 radix_sort vs std::sort/stability");
    struct E {
        std::uint64_t hi, lo;
        std::uint32_t p;
    };
    for (int it = 0; it < iter; ++it) {
        const int n = 1 + g.rng.below(2000);
        std::vector<E> a(static_cast<std::size_t>(n));
        for (auto& e : a) {
            e.hi = g.rng.next() % 250;
            e.lo = g.rng.next() % 250;
            e.p = g.rng.u32();
        }
        std::vector<E> b = a;
        std::vector<E> tmp;
        radix_sort::sort(a, tmp, [](const E& e) { return e.hi; },
                         [](const E& e) { return e.lo; },
                         [](const E& e) { return e.p; });
        std::stable_sort(b.begin(), b.end(), [](const E& x, const E& y) {
            if (x.hi != y.hi) return x.hi < y.hi;
            if (x.lo != y.lo) return x.lo < y.lo;
            return x.p < y.p;
        });
        bool same = a.size() == b.size();
        for (int i = 0; i < n && same; ++i)
            if (a[static_cast<std::size_t>(i)].hi != b[static_cast<std::size_t>(i)].hi ||
                a[static_cast<std::size_t>(i)].lo != b[static_cast<std::size_t>(i)].lo ||
                a[static_cast<std::size_t>(i)].p != b[static_cast<std::size_t>(i)].p)
                same = false;
        CHECK(same, "radix_sort output != stable reference (n=%d it=%d)", n, it);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Gen g(0xC0FFEE12345ULL);
    std::printf("== mss review differential harness ==\n");
    testBasicAnalyzer(g, 400);
    std::printf("T1 done\n");
    testBasicUpdater(g, 200);
    std::printf("T2 done\n");
    // T3（Structure::Updater）已被 repro2 证实存在确定性崩溃 bug（疑似区外 vis/cellHash 残留），
    // 独立进程复现；这里跳过以免中断整轮 diff 测试。
    std::printf("T3 skipped (confirmed library bug, see repro2)\n");
    testExactAnalyze(g, 500);
    std::printf("T4 done\n");
    testExactObserve(g, 300);
    std::printf("T5 done\n");
    testRadixSort(g, 60);
    std::printf("T7 done\n");
    testEndgame(g, 120, false);
    std::printf("T6 done\n");
    testEndgame(g, 400, true);  // T8 更密集搜索
    std::printf("T8 done\n");
    std::printf("== done: %d checks, %d fails ==\n", T::checks, T::fails);
    return T::fails == 0 ? 0 : 1;
}