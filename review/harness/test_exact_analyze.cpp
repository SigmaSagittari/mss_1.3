#include "common.h"
#include "tests.h"

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

void testExactAnalyze(Gen& g, int iter) {
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
