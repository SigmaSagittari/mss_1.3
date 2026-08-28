#include "common.h"
#include "tests.h"

// ── 测试 8：EndgameBruteforce vs 朴素最优解（诚实概率网格，真实 digit）──
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

void testEndgame(Gen& g, int iter) {
    T::section("T8 Endgame honest-prob-grid (P==1 exclusion)");
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

        // 概率网格：真实 per-cell（候选 = p<1.0 的非必雷格）
        Grid<long double> probGrid(rows, cols, 0.0L);
        std::vector<char> isCandidate(static_cast<std::size_t>(rows * cols), 0);
        for (const auto [i, j] : hidden) {
            const long double p = info.total
                ? static_cast<long double>(info.mineCount[static_cast<std::size_t>(rb.flat(i, j))]) /
                      static_cast<long double>(info.total)
                : 0.0L;
            probGrid[i][j] = p;
            const bool cand = p < 1.0L;
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
                    std::printf("  [FAIL-T8] move#%d lib=%d naive=%d it=%d (best lib=%d naive=%d)\n",
                                j, libW, naiW,
                                it, libBest(lib), naiveBest);
            }
        }
        CHECK(sameWins, "per-move win mismatch it=%d", it);
    }
}
