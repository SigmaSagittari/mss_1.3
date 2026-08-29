#include <map>
#include <utility>
#include "common.h"
#include "tests.h"

// ── 测试 10：Probability::Result::frontierCells 与暴力扫描对照 ──
// 暴力参照：逐格调用 mineProbability（全部前沿格）过滤 < p。
// 校验：返回集合与参照完全一致（无序集合比对、值逐项相等），
// 只含前沿格、无重复、概率严格低于阈值。

void testFrontierCells(Gen& g, int iter) {
    T::section("T10 frontierCells == brute force scan");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3 + g.rng.below(4), cols = 3 + g.rng.below(4);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        ObservedBoard b = toLibBoard(rb);
        Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool pool;
        Structure::Result st = Structure::Analyzer::analyze(b, basic, pool);
        Distribution::DistPool dpool;
        const Probability::Result r = Exact::analyze(b, basic, st, dpool);

        // 阈值：偶尔取 0（空集边界），其余 [0, 1.2)（覆盖 p>1 全量边界）。
        const long double p = g.rng.below(50) == 0
                                  ? 0.0L
                                  : (1.2L * static_cast<long double>(g.rng.below(1000)) /
                                     1000.0L);

        // 暴力参照：全部前沿格逐格查询。
        std::map<std::pair<int, int>, long double> expect;
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j) {
                if (basic.marks[i][j] != Basic::Mark::Frontier) continue;
                const long double mp =
                    r.mineProbability(b.id(i, j), b, basic, st);
                if (mp < p) expect[{i, j}] = mp;
            }

        const std::vector<Probability::Result::FrontierCell> got =
            r.frontierCells(b, st, p);

        // 单条记录：只含前沿格、概率严格低于阈值、无重复。
        std::map<std::pair<int, int>, long double> gmap;
        for (const auto& c : got) {
            CHECK(basic.marks[c.x][c.y] == Basic::Mark::Frontier,
                  "T10: non-frontier cell returned it=%d (%d,%d)", it, c.x, c.y);
            CHECK(c.p < p, "T10: prob not below threshold it=%d p=%.3Lf got=%.3Lf", it,
                  p, c.p);
            const bool inserted =
                gmap.emplace(std::make_pair(c.x, c.y), c.p).second;
            CHECK(inserted, "T10: duplicate cell it=%d (%d,%d)", it, c.x, c.y);
        }

        // 与参照逐项一致（同一 boxProbs 来源，值应 bitwise 相等）。
        CHECK(gmap.size() == expect.size(),
              "T10: size mismatch it=%d p=%.3Lf got=%zu expect=%zu", it, p,
              gmap.size(), expect.size());
        CHECK(gmap == expect, "T10: set mismatch it=%d p=%.3Lf", it, p);
    }
}