#include "common.h"
#include "tests.h"

// ── 测试 5：Exact::observe vs 参考枚举 ──
void testExactObserve(Gen& g, int iter) {
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
