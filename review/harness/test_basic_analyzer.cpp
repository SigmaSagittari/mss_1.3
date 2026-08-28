#include "common.h"
#include "tests.h"

// ── 测试 1：Basic::Analyzer vs 参考标记 ──
void testBasicAnalyzer(Gen& g, int iter) {
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
