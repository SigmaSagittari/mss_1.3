#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>
#include <vector>

#include "analysis/distribution/distribution.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/workspaces.h"

namespace mss {
// 本模块工作区（core/workspaces.h 集中存放，全程序每线程一份）。
using scratch::exactWs;

// ─────────────────────────────────────────────────────────────
// exact.h — 精确概率引擎（无近似；残局/根节点的无偏参考实现）。
//
// 方法：把每个连通块的分布写成生成函数（多项式），相乘得全块联合分布，
// 再用组合数把"非前沿格"（T 格）的雷数纳入，得到每格的精确雷概率。
//
// 内部细节（Poly、阶乘表、combLog）全部为 Exact 私有，不暴露。
// ─────────────────────────────────────────────────────────────

struct Exact {
    // 精确分析：吃活组件 + 分布池，输出可查询的 Result。
    static Probability::Result analyze(const ObservedBoard& board,
                                       const Basic::Result& basic,
                                       const Structure::Result& structure,
                                       Distribution::DistPool& pool);

    // 观察：点开格子 cell 的结果分布（爆炸概率 + 数字 0..8 概率）。
    // explosion = prob.mineProbability(cell)（P(x 是雷)，observe 不自算雷概率）；
    // 已揭示/数字格 → 全 0。算法分七步（见实现内注释）：被抓住部分折成
    // 稀疏转移表做 dp，其余棋盘折成 f[t]，预算卷积后归一化。
    static Probability::ObserveResult observe(const ObservedBoard& board,
                                              const Basic::Result& basic,
                                              const Structure::Result& structure,
                                              const Probability::Result& prob,
                                              Distribution::DistPool& pool, CellId cell);

private:
    // 阶乘对数表（线程局部，惰性扩展）。
    static std::vector<long double>& logFactorial();
    static void combiInit(int n);
    // 组合数（用阶乘对数避免溢出）。调用方保证 0<=k<=n；越界=调用 bug。
    static long double combLog(int n, int k);

    // 多项式容器（定义在 workspaces.h 的 ExactWs 内嵌套；本类内以 Poly 使用）。
    using Poly = ExactWs::Poly;

    // 候选方案数：把非前沿格的雷数组合也纳入。
    static long double denominator(const Poly& gf, int totalMines, int tSum);

    // ── 多项式运算（复用缓冲版，语义与原 operator*/operator//Polynomial(dist) 一致）──
    // dist → out（min/max 单遍扫描 + 系数铺填）。
    static void polyFromDist(const Distribution& dist, Poly& out);
    // acc *= src：结果写进 mult 轮转缓冲后与 acc 交换（mult 留下旧缓冲供
    // 下轮复用，容量保留）。调用方保证 acc/src/mult 互不别名。
    static void polyMulFrom(Poly& acc, const Poly& src, Poly& mult);
    // q = a / b（精确长除，含近零跳步，语义同旧 operator/）；rem 为过程
    // 缓冲。调用方保证 a/b/q/rem 互不别名。
    static void polyDivideFrom(const Poly& a, const Poly& b,
                               Poly& q, Poly& rem);
};

// ── 实现区 ──

inline std::vector<long double>& Exact::logFactorial() {
    static thread_local std::vector<long double> table;
    return table;
}

inline void Exact::combiInit(int n) {
    auto& t = logFactorial();
    if (t.empty()) t.push_back(0.0);
    while (static_cast<int>(t.size()) <= n + 1)
        t.push_back(t.back() + std::log(static_cast<long double>(t.size())));
}

inline long double Exact::combLog(int n, int k) {
    // 调用方（denominator/analyze）均先检查 lightMines 在 [0, tSum] 内；
    // 越界到达这里 = 内部 bug。
    assert_(k >= 0 && k <= n, "Exact::combLog: 参数越界");
    if (k == 0 || k == n) return 1;
    combiInit(n);
    return std::exp(logFactorial()[n] -
                    logFactorial()[k] -
                    logFactorial()[n - k]);
}

inline void Exact::polyFromDist(const Distribution& dist, Exact::Poly& out) {
    // 活组件的分布必有可行雷数（waySum > 0）；空分布 = shape/分布层 bug。
    assert_(!dist.entries.empty(), "Exact::polyFromDist: 空分布");
    int minExp = dist.entries[0].mineCount;
    int maxExp = minExp;
    for (const auto& d : dist.entries) {
        minExp = std::min(minExp, d.mineCount);
        maxExp = std::max(maxExp, d.mineCount);
    }
    out.start = minExp;
    out.coeffs.assign(maxExp - minExp + 1, 0.0);
    for (const auto& d : dist.entries)
        out.coeffs[d.mineCount - minExp] = d.ways;
}

inline void Exact::polyMulFrom(Exact::Poly& acc, const Exact::Poly& src,
                               Exact::Poly& mult) {
    const int size = static_cast<int>(acc.coeffs.size()) +
                     static_cast<int>(src.coeffs.size()) - 1;
    mult.coeffs.assign(size, 0.0);
    for (int i = 0; i < static_cast<int>(acc.coeffs.size()); ++i)
        for (int j = 0; j < static_cast<int>(src.coeffs.size()); ++j)
            mult.coeffs[i + j] +=
                acc.coeffs[i] *
                src.coeffs[j];
    mult.start = acc.start + src.start;
    acc.coeffs.swap(mult.coeffs);
    acc.start = mult.start;
}

inline void Exact::polyDivideFrom(const Exact::Poly& a, const Exact::Poly& b,
                                  Exact::Poly& q, Exact::Poly& rem) {
    const int asz = static_cast<int>(a.coeffs.size());
    const int bsz = static_cast<int>(b.coeffs.size());
    q.start = a.start - b.start;
    q.coeffs.assign(std::max(0, asz - bsz + 1), 0.0);
    rem.coeffs.assign(a.coeffs.begin(), a.coeffs.end());
    const long double bLead = b.coeffs.back();
    for (int i = asz - 1; i >= bsz - 1; --i) {
        if (std::abs(rem.coeffs[i]) < 1e-10L) continue;
        const long double factor = rem.coeffs[i] / bLead;
        q.coeffs[i - (bsz - 1)] = factor;
        for (int j = 0; j < bsz; ++j)
            rem.coeffs[i - j] -=
                factor * b.coeffs[bsz - 1 - j];
    }
}

inline long double Exact::denominator(const Exact::Poly& gf, int totalMines, int tSum) {
    long double result = 0.0;
    for (int i = 0; i < static_cast<int>(gf.coeffs.size()); ++i) {
        const int heavyMines = gf.start + i;
        const int lightMines = totalMines - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum)
            result += gf.coeffs[i] * combLog(tSum, lightMines);
    }
    return result;
}

inline Probability::Result Exact::analyze(const ObservedBoard& board,
                                          const Basic::Result& basic,
                                          const Structure::Result& structure,
                                          Distribution::DistPool& pool) {
    const int M = board.totalMines - basic.mineSum;
    const int tSum = basic.unknownSum;

    // 活组件：收集分布，对齐到 Result.components（下标 = ComponentId）。
    // aliveIds 原是 0..n-1 的恒等序列，直接以下标即 ComponentId 遍历。
    Probability::Result result;
    result.components.resize(structure.components.size());

    exactWs.distList.clear();
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid)
        // 取分布（同 shape 幂等命中池缓存）。
        exactWs.distList.push_back(
            Distribution::Solver::analyze(*structure.components[cid].shape, pool));

    // 全块联合生成函数（多项式缓冲在 core/workspaces.h 的 exactWs，复用零分配）。
    exactWs.pH.start = 0;
    exactWs.pH.coeffs.assign(1, 1.0);
    for (const auto* d : exactWs.distList) {
        polyFromDist(*d, exactWs.pi);
        polyMulFrom(exactWs.pH, exactWs.pi, exactWs.mult);
    }

    const long double denom = denominator(exactWs.pH, M, tSum);

    // Unknown 格是雷的概率：总雷数 - 1 分配给其余格子。
    long double lightProb = 0.0;
    for (int i = 0; i < static_cast<int>(exactWs.pH.coeffs.size()); ++i) {
        const int heavyMines = exactWs.pH.start + i;
        const int lightMines = M - 1 - heavyMines;
        if (lightMines >= 0 && lightMines <= tSum - 1)
            lightProb += exactWs.pH.coeffs[i] * combLog(tSum - 1, lightMines);
    }
    lightProb /= denom;
    result.tCellProbability = Probability::limitProbability(lightProb);
    result.candidates = denom;

    // 每连通块：各分布的取到概率 → 每 box 的雷概率（均摊）。
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst = structure.components[cid];
        const Distribution& dist =
            *exactWs.distList[static_cast<std::size_t>(cid)];
        polyFromDist(dist, exactWs.pi);
        polyDivideFrom(exactWs.pH, exactWs.pi, exactWs.ti, exactWs.rem);  // 去掉该块后的联合分布

        exactWs.boxProb.assign(dist.entries.size(), 0.0L);
        for (std::size_t j = 0; j < dist.entries.size(); ++j) {
            const int v = dist.entries[j].mineCount;
            const long double w = dist.entries[j].ways;
            long double numerator = 0.0L;
            for (int k = 0; k < static_cast<int>(exactWs.ti.coeffs.size()); ++k) {
                const int tMines = exactWs.ti.start + k;
                const int lightMines = M - v - tMines;
                if (lightMines >= 0 && lightMines <= tSum)
                    numerator += w * exactWs.ti.coeffs[k] *
                                 combLog(tSum, lightMines);
            }
            exactWs.boxProb[j] = numerator / denom;
        }

        // 每 box 期望 × 取到概率 → 均摊到单格。
        auto& cr = result.components[cid];
        const Structure::Shape& shape = *inst.shape;
        cr.boxProbs.resize(shape.boxes.size());
        for (std::size_t b = 0; b < shape.boxes.size(); ++b) {
            long double perCell = 0.0L;
            for (std::size_t j = 0; j < dist.entries.size(); ++j)
                perCell += exactWs.boxProb[j] * dist.entries[j].perBoxExpectation[b];
            cr.boxProbs[b] = perCell / static_cast<long double>(shape.boxes[b].size);
        }
    }

    return result;
}

// ── observe 实现 ──
// 纯查询（零修改）：爆炸概率 = prob.mineProbability(cell)，直接取自 analyze
// 的雷概率（已含全局预算卷积），这里只算数字分布。算法：
//   把"被抓住部分"（x 邻居所属连通块 ∪ x 所在连通块 ∪ T 邻居伪源）预计算成
//   稀疏转移表 (h, y, w)（h = 邻域雷数贡献，y = 块雷数，w = 摆法数），
//   dp 逐表卷积得 dp[x][y]（x = 邻居雷数，y = 被抓住部分雷数）；其余棋盘
//   折成 f[t]（恰有 t 雷的摆法数）；最后按预算 y+t=M 卷积并归一化。
inline Probability::ObserveResult Exact::observe(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, const Probability::Result& prob,
    Distribution::DistPool& pool, CellId cell) {
    using Mark = Basic::Mark;
    const auto [x, y] = board.pos(cell);
    const int rows = board.rows, cols = board.cols;
    const int tSum = basic.unknownSum;
    const int M = board.totalMines - basic.mineSum;

    Probability::ObserveResult out;

    // 边界：已揭示格不可开 → 全 0；basic 已定雷 → explosion = 1（无数字）。
    if (board.board[x][y] != Cell::Hidden) return out;
    if (basic.marks[x][y] == Mark::Mine) {
        out.explosion = 1.0L;
        return out;
    }
    const bool xInT = (basic.marks[x][y] == Mark::Unknown);
    const CellLocation xloc = structure.cellLoc[cell];
    const bool xInBox = (xloc.component >= 0);
    const ComponentId xCid = xloc.component;
    const BoxId xBox = xloc.box;

    // ── 步骤 1：被抓住集合 = 邻居所属连通块 ∪ {x 所在连通块} ∪ T 邻居 ──
    // 邻居分类：basic Mine → fixed 源（常数）；Unknown → T 邻居（伪源）；
    //           Frontier → 所属连通块；已揭示数字 / Safe → 无贡献。
    // x 所在连通块不必显式加入：x 是 Frontier ⟹ 邻接已揭示数字，且该数字属于
    // x 的连通块（cellLoc 回填），邻居循环必然捕获它；下方断言兜底。
    // 必须捕获它的原因：x 的安全因子（池子 s−1）住在它的 box 转移里。
    int fixed = 0;
    int uT = 0;
    // 复用工作区（core/workspaces.h，调用期使用，开头重置）。
    exactWs.captured.clear();
    exactWs.seen.assign(structure.components.size(), 0);
    exactWs.tran.clear();
    exactWs.tranRanges.clear();
    forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
        if (basic.marks[nx][ny] == Mark::Mine) {
            ++fixed;
            return;
        }
        if (basic.marks[nx][ny] == Mark::Unknown) {
            ++uT;
            return;
        }
        const CellLocation loc = structure.cellLoc[board.id(nx, ny)];
        if (loc.component == -1) return;  // 已揭示数字 / Safe
        if (!exactWs.seen[loc.component]) {
            exactWs.seen[loc.component] = 1;
            exactWs.captured.push_back(loc.component);
        }
    });
    // x 在 box 里却未被捕获 = Basic/Structure 跨层不变式被破坏 → 概率静默错误。
    assert_(!xInBox || exactWs.seen[xCid] != 0,
            "Exact::observe: x 所在连通块未被捕获");

    // ── 步骤 2：转移预计算 —— 每个被抓住连通块折成一张稀疏转移表 ──
    // 每张表是 (h, y, w) 列表（平铺在 exactWs.tran，ranges 记区间）：邻域
    // 雷数贡献 h、块雷数 y、摆法数 w。超几何/box 细节只活在这里，dp 只吃表。
    // maxY 为 dp 的 y 上限：被抓住部分总格数（T 伪源 u_T 格 + 被抓住各 box
    // 格数）。x 恒非雷（爆炸概率取自 prob），其雷数不进 y。
    int maxY = uT;
    for (ComponentId cid : exactWs.captured) {
        const Structure::Instance& inst = structure.components[cid];
        const Structure::Shape& shape = *inst.shape;
        for (const auto& box : shape.boxes) maxY += box.size;
        // 各 box 中与 x 相邻的格数（observe 内部预计算用）
        exactWs.u.assign(shape.boxes.size(), 0);
        for (std::size_t b = 0; b < shape.boxes.size(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [cx, cy] = board.pos(inst.boxes.cells[k]);
                if (std::abs(cx - x) <= 1 && std::abs(cy - y) <= 1 &&
                    !(cx == x && cy == y))
                    ++exactWs.u[b];
            }
        // 枚举 box 雷数分配：y 由分配确定，邻域贡献是各相邻 box 超几何的卷积。
        // 权重 = ways × conv[h]：ways 含非相邻 box 的 C(s,m)，conv 把相邻 box
        // 的 C(s,m) 抵消、换成受限选格数（x 所在 box 池子恒为 s−1）。
        int maxTotal = 0;
        for (const auto& box : shape.boxes) maxTotal += box.size;
        exactWs.acc.assign(maxTotal + 1, {});
        Distribution::Solver::forEachAssignment(
            shape, [&](const std::vector<char>& assignment, long double ways) {
                int yInc = 0;
                std::array<long double, 9> conv{};
                conv[0] = 1.0L;
                for (std::size_t b = 0; b < assignment.size(); ++b) {
                    const int m = assignment[b];
                    yInc += m;
                    const int ub = exactWs.u[b];
                    const bool isXBox = (cid == xCid &&
                                         static_cast<BoxId>(b) == xBox);
                    if (ub == 0 && !isXBox) continue;  // 不邻接且非 x 的 box：只进 y
                    const int s = shape.boxes[b].size;
                    const int pool = isXBox ? s - 1 : s;  // x 恒非雷：剔除 x
                    std::array<long double, 9> dist{};
                    const int rMax = (std::min)(ub, m);
                    for (int r = 0; r <= rMax; ++r) {
                        const int rest = m - r;
                        if (rest > pool - ub) continue;
                        dist[r] =
                            combLog(ub, r) * combLog(pool - ub, rest) /
                            combLog(s, m);
                    }
                    // 卷积：conv ⊗ dist（h 平移 r）
                    std::array<long double, 9> nc{};
                    for (int h = 0; h <= 8; ++h)
                        if (conv[h] != 0.0L)
                            for (int r = 0; r <= 8 - h; ++r)
                                nc[h + r] +=
                                    conv[h] *
                                    dist[r];
                    conv = nc;
                }
                for (int h = 0; h <= 8; ++h)
                    if (conv[h] != 0.0L)
                        exactWs.acc[yInc][h] +=
                            ways * conv[h];
            });
        // 折成平铺转移表：acc 稠密行 → 稀疏 (h, y, w) 三元组。
        const int off = static_cast<int>(exactWs.tran.size());
        for (int t = 0; t <= maxTotal; ++t)
            for (int h = 0; h <= 8; ++h) {
                const long double w = exactWs.acc[t][h];
                if (w != 0.0L) exactWs.tran.push_back(ExactWs::Transfer{h, t, w});
            }
        exactWs.tranRanges.emplace_back(off,
                                        static_cast<int>(exactWs.tran.size()) - off);
    }
    // T 邻居伪源：u_T 个 size-1 box（每格 0/1 雷），同为一张 (r, r, C(u_T,r)) 表。
    // 预算不在此约束，由步骤 5 的 f[M−y] 兜底。
    if (uT > 0) {
        const int off = static_cast<int>(exactWs.tran.size());
        for (int r = 0; r <= uT; ++r)
            exactWs.tran.push_back(ExactWs::Transfer{r, r, combLog(uT, r)});
        exactWs.tranRanges.emplace_back(off,
                                        static_cast<int>(exactWs.tran.size()) - off);
    }

    // ── 步骤 3：dp[x][y] —— 依次应用每张转移表 ──
    // dp 语义：x = 邻居雷数(0..8)，y = 被抓住部分总雷数(0..maxY)，值为方案数。
    const int stride = maxY + 1;
    auto runDp = [&]() -> const std::vector<long double>& {
        exactWs.dp.assign(9 * stride, 0.0L);
        exactWs.dp[0 * stride + 0] = 1.0L;
        for (const auto& [off, cnt] : exactWs.tranRanges) {
            exactWs.ndp.assign(9 * stride, 0.0L);
            for (int t = 0; t < cnt; ++t) {
                const ExactWs::Transfer& tr = exactWs.tran[off + t];
                for (int xv = 0; xv + tr.h <= 8; ++xv)
                    for (int yv = 0; yv + tr.y <= maxY; ++yv) {
                        const long double base = exactWs.dp[xv * stride + yv];
                        if (base == 0.0L) continue;
                        exactWs.ndp[(xv + tr.h) * stride + yv + tr.y] += base * tr.w;
                    }
            }
            exactWs.dp.swap(exactWs.ndp);
        }
        return exactWs.dp;
    };

    // ── 步骤 4：f[t] —— 其余部分（非被抓住连通块 + 剩余 T 格）恰有 t 雷的摆法数 ──
    // T 池子 = tSum − u_T − [x∈T]：查询格 x 永不进其余部分，x∈T 时自身再剔除。
    // f 的下标范围到 tPool + 非抓住组件最大雷数（组件格子也能吃雷）。
    const int tPool = tSum - uT - (xInT ? 1 : 0);
    exactWs.pRest.start = 0;
    exactWs.pRest.coeffs.assign(1, 1.0);
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(structure.components.size());
         ++cid) {
        const Structure::Instance& inst = structure.components[cid];
        if (exactWs.seen[cid]) continue;
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, pool);
        polyFromDist(*dist, exactWs.pi);
        polyMulFrom(exactWs.pRest, exactWs.pi, exactWs.mult);
    }
    const int pRestMax =
        exactWs.pRest.coeffs.empty()
            ? 0
            : exactWs.pRest.start + static_cast<int>(exactWs.pRest.coeffs.size()) - 1;
    exactWs.f.assign(tPool + pRestMax + 1, 0.0L);
    for (std::size_t i = 0; i < exactWs.pRest.coeffs.size(); ++i) {
        const long double w = exactWs.pRest.coeffs[i];
        if (w == 0.0L) continue;
        const int t1 = exactWs.pRest.start + static_cast<int>(i);
        // T 格最多吃 tPool 个雷（r 是 T 格雷数；组件雷数 t1 不受 tPool 限制）
        for (int r = 0; r <= tPool; ++r)
            exactWs.f[t1 + r] += w * combLog(tPool, r);
    }

    // ── 步骤 5：dp 与 f 的预算卷积（y + t = M），按邻居雷数分组 ──
    const int fMax = static_cast<int>(exactWs.f.size()) - 1;
    const std::vector<long double>& dpSafe = runDp();  // 引用复用缓冲，零拷贝
    std::array<long double, 9> F{};
    for (int xv = 0; xv <= 8; ++xv)
        for (int yv = 0; yv <= maxY; ++yv) {
            const long double d = dpSafe[xv * stride + yv];
            if (d == 0.0L) continue;
            const int t = M - yv;
            if (t < 0 || t > fMax) continue;
            F[xv] += d * exactWs.f[t];
        }

    // ── 步骤 6：explosion —— P(x 是雷) 直接取自 analyze 的雷概率 ──
    // （mineProbability 已含全局预算卷积；observe 的 DP 只算 x 非雷的配置，
    //   故 Σdigit + explosion = 1 由 N = ΣF + E 保证。）
    out.explosion = prob.mineProbability(cell, board, basic, structure);

    // ── 步骤 7：归一化（N = 全盘方案数，T 池子用 tSum，x 允许是雷）──
    exactWs.pAll.coeffs.assign(exactWs.pRest.coeffs.begin(),
                               exactWs.pRest.coeffs.end());  // 复制起步（f 已用完 pRest）
    exactWs.pAll.start = exactWs.pRest.start;
    for (ComponentId cid : exactWs.captured) {
        const Structure::Instance& inst = structure.components[cid];
        const Distribution* dist = Distribution::Solver::analyze(*inst.shape, pool);
        polyFromDist(*dist, exactWs.pi);
        polyMulFrom(exactWs.pAll, exactWs.pi, exactWs.mult);
    }
    const long double N = denominator(exactWs.pAll, M, tSum);
    for (int xv = 0; xv <= 8 && fixed + xv <= 8; ++xv)
        out.digit[fixed + xv] =
            F[xv] / N;
    return out;
}

}  // namespace mss
