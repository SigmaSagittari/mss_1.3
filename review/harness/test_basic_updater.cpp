#include "common.h"
#include "tests.h"

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

void testBasicUpdater(Gen& g, int iter) {
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

        int steps = 1 + g.rng.below(6);
        std::vector<Basic::Delta> deltas;
        std::vector<Basic::Update> allUpdates;
        for (int s = 0; s < steps; ++s) {
            const int failsBefore = T::fails;
            // 随机选一个非雷隐藏格揭示（只支持揭示更新：回滚已被库断言禁止）
            std::vector<std::pair<int, int>> hidden;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const bool isMineCell =
                        trueMine[static_cast<std::size_t>(rb.flat(i, j))];
                    if (b.board[i][j] == Cell::Hidden && !isMineCell)
                        hidden.emplace_back(i, j);
                }
            if (hidden.empty()) { steps = s; break; }  // 无可揭示格：提前结束本盘步骤
            std::vector<Basic::Update> ups;
            {
                const auto [i, j] = hidden[g.rng.below(static_cast<int>(hidden.size()))];
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (trueMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++v;
                });
                b.board[i][j] = toCell(v);
                ups.push_back({b.id(i, j), b.board[i][j]});
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
