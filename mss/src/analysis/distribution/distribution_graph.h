#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

// 测试专用访问口（友元）：实现定义于 test/distribution/greedy_order.h。
// 生产构建（不含测试）下该名字只保持前置声明，不生成任何代码。
namespace test {
struct OrderProbe;
}  // namespace test

// ─────────────────────────────────────────────────────────────
// distribution_graphv.h — 基于图分解的组件分布求解（设计稿）。
//
// 0. 输入模型。
//    Basic 已完成确定安全/地雷的传播；Structure::Shape 把剩余 Frontier 格
//    按相同数字邻域合并成 box。令 x_b 为 box b 的雷数，则
//
//      0 <= x_b <= size(b),    weight_b(x_b) = C(size(b), x_b)
//      Σ_{b in N(c)} x_b = sum(c)                 （每个数字约束 c）
//
//    一个组件的 Distribution 要返回：
//      Z[t]     = Σ valid x, Σx=t ∏ weight_b(x_b)
//      M[b][t]  = Σ valid x, Σx=t x_b ∏ weight_b(x_b)
//    因而 Entry::ways = Z[t]，perBoxExpectation[b] = M[b][t] / Z[t]。
//    Exact 只依赖这两个量做全盘雷数卷积；它不需要列出具体摆雷盘面。
//
// 1. 图。
//    对 box 建 primal graph：两个 box 同时出现于一个数字约束时连边。
//    Shape 的普通连通块只是这个图的 connected component；直接 box DFS 的
//    复杂度可能随整个 connected component 指数增长。
//
// 2. 第一层分解：articulation box / block-cut tree。
//    删除一个割 box 后，剩余图若裂成多个分量，则固定该 box 的 x_b 后，各
//    分量条件独立。点双连通 block 与割 box 组成 block-cut tree：
//
//               [block] -- (cut box) -- [block]
//
//    block 是局部 CSP，cut box 的状态是 k in [0, size(box)]。box 的组合
//    权重只能在其自身节点计一次，不能被每个相邻 block 重复乘入。
//
// 3. 一般化：recursive separator DP。
//    对难以由单割点拆开的 block，选择小 box separator S；固定 x_S 后，
//    删除 S 的各子区独立。一个 region 的消息为
//
//      F_region(x_boundary, t) = 给定边界 box 雷数、内部共 t 雷的加权方案数。
//
//    子消息按 t 卷积、按共享 boundary 状态相乘并求和。割点是 |S|=1 的特例；
//    环通常可由 |S|=2 的 separator 打开。真正的指数项是任意递归层最大
//    boundary 的状态数 Π_b(size(b)+1)，即 treewidth 风格的宽度，而不是
//    单次最小割的大小。
//
// 4. 局部求解与兜底。
//    小 block 继续使用现有 box DFS；找不到小 separator 的大双连通核也走
//    同一兜底。树分解只减少计数成本，不负责物化每一个具体地雷盘面。
//    旧 all_distribute / 残局暴力枚举应保留独立的枚举路径：输出 N 个盘面
//    本身就有 Ω(N) 成本，不能由 Distribution DP 消掉。
//
// 5. separator 选择启发式。
//    线性展开时维护已展开 P 与未展开 U 的 frontier：P 中仍邻接 U 的 box。
//    选候选 v 的即时状态代价变化为
//
//      Δlog(v) = [rem(v)>0] log(size(v)+1)
//                - Σ_{u in N(v)∩P, rem(u)=1} log(size(u)+1)
//
//    rem(u) 是 u 当前仍在 U 的邻居数。每步选最小 Δlog 的候选，是对最小
//    frontier-state 展开的局部贪心；所有更新只涉及 v 的常数度邻域，可用
//    优先队列增量维护。若未展开图裂开，应直接成为独立 child region，而
//    不应为维持单一 source/sink 顺序强行禁止这种分裂。
//
// 实施边界：本文件仅承载图分解版 analyze；既有 Distribution / Exact 接口
// 保持不变，forEachAssignment / all_distribute 继续属于枚举慢路径。
// ─────────────────────────────────────────────────────────────

struct DistributionSolver {
    // ── 数据类 ──

    // 单种雷数的分布。
    struct Distribution {
        struct Entry {
            int mineCount = 0;                           // 连通块总雷数
            long double ways = 0;                        // 摆放方案数
            std::vector<long double> perBoxExpectation;  // 每单位格期望雷数（BoxId 局部）
        };
        std::vector<Entry> entries;
    };
    // ── 池 ──

    // 分布池：shape.hash（内容指纹）→ const Distribution*，只增不删。
    // 键 = shape.hash（Structure::ShapePool::intern 时写入）：同内容必同分布，
    // 跨盘面 / 跨 ShapePool 命中合法。不能用 Shape* 指针做键：指针只在所属
    // ShapePool 生命周期内稳定，跨盘面新建池时堆地址会被 malloc 复用。
    struct DistPool {
        const Distribution* get(const Structure::Shape* shape);
        const Distribution* get(const Structure::Shape* shape) const;
        const Distribution* insert(const Structure::Shape* shape, Distribution dist);

    private:
        std::vector<std::unique_ptr<Distribution>> dists_;  // 稳定地址，只增不删
        FlatHashTable<U128, const Distribution*, U128Hash> index_;
    };

    // ── 展开序 ──
    // 展开恒用私有 Graph 产线的 lexBfsOrder（起点经 diameterStart 选取）；
    // 抛光方式由 analyze 的模板枚举在编译期选定，默认相邻抛光。产线锁在
    // Graph 内部，测试经友元 OrderProbe 组装两种抛光基准。
    enum class PolishKind { Adjacent, Window3 };

    // 汇总分布表（forEachAssignment 聚合），经 DistPool 去重缓存。
    template <PolishKind polish = PolishKind::Adjacent>
    static const Distribution* analyze(const Structure::Shape& shape, DistPool& pool);

private:
    // ── 图 ──
    // 仅按 Shape 的 BoxId 编号；纯内部实现细节，不对外暴露。展开序产线
    // （直径端点、lexBFS、两种抛光）是图自己的子函数。Graph 整体归 solver
    // 私有持有，父子一体，方法成员直接可见，无需友元。
    struct Graph {
        // 仅按 Shape 的 BoxId 编号（测试探针要直接组装/校验，公开）。
        std::vector<std::vector<BoxId>> neighbors;

        // 展开起点：从 box 0 出发的 DFS 最远端点（启发式；原"双扫"的第二段
        // 端点无人使用，已删）。测试探针取 init 用，公开。
        BoxId diameterStart() const;

        // 对外只暴露两个成品入口：默认先用 lexBfsOrder(diameterStart())
        // 取展开序再抛光。

        // 相邻交换抛光成品（默认 2 轮）。
        std::vector<BoxId> polishAdjacent(BoxId init) const;

        // 3 窗口全排列抛光成品。
        std::vector<BoxId> polishWindow3(BoxId init) const;

    private:
        // LexBFS 展开序（起点经 diameterStart 选取）：只被默认抛光入口
        // 调用，不外露。
        std::vector<BoxId> lexBfsOrder(BoxId init) const;

        // 抛光核心（相邻交换 rounds 轮，默认 2 / 3 窗口全排列）：给定 order
        // 就地抛光后返回。只被上面的默认入口调用，不外露。
        std::vector<BoxId> polishAdjacent(std::vector<BoxId> order, int rounds = 2) const;
        std::vector<BoxId> polishWindow3(std::vector<BoxId> order) const;
    };

    // ── 前沿 DP：状态压缩到"前沿"，SoA 平铺 ──
    //
    // 干什么：求一个组件的分布表 Z[t] / M[b][t]，替代暴力枚举。逐层沿
    // order 展开，每层维护一张状态表，一个状态 = 一个可合并的部分历史，
    // 值 = 该历史累计到当前的摆法权重。把"枚举全部 x 组合"换成"逐层
    // 枚举 + 状态合并"，复杂度由同层状态数（前沿宽度）主导。
    //
    // 状态表示（F1 重写后的核心）：
    //   只有"仍被未来约束读取"的 box 值有意义 —— 关闭/未赋值的 box 槽
    //   恒为 0。因此状态压成 packed[|F|]：只存当前层未关闭 box 的雷数。
    //   boxId ↔ 槽位 的映射**不存进状态**：open 集每层都相同（是形状的
    //   静态性质，由 order 的 position 与 closeStep 唯一确定），由
    //   analysis 逐层流式构建的 layout/slotOf/gather 表提供；转移只是一
    //   次按 gather 表的逐槽拷贝（幸存者继承旧槽，v 恒追加在末尾）。
    //
    //   合并判据 = mineCount（累计雷数 t）+ packed 整体相等。关闭/未赋值
    //   槽恒为 0 且每层所有状态相同，不参与区分 —— 与旧实现"全向量相等"
    //   完全等价的判据，只是不再反复搬 n 个槽。
    //
    // 封口与关闭都是静态的，由 order 位置推出：
    //   封口：约束 c 的成员在 order 中的最晚位置 lastPos，层 k == lastPos
    //         时 c 封口，唯一合法 k 被夹死（见 Check::remAfter）；
    //   关闭：box b 的关闭层 closeStep = b 所有约束 lastPos 的最大值，
    //         因为 b 需要保留 ⟺ 它还有约束未封口。
    //
    // M 通道：columns[state][box]，列 = Σ(ways × x)：box 关闭那一层写入
    // x × 分支权重，之后随分支缩放（未关闭列恒 0，缩放/合并循环只走
    // "已关闭"列，closedSoFar 静态维护），末层 ÷ ways 得 perBoxExpectation。
    //
    // 平铺与复用：一层用 SoA（ways/columns/mineCounts/packed + 哈希索引），
    // 无逐状态 vector/堆分配；两层缓冲 thread_local 交替复用跨调用容量。
    class dpHelper {
    public:
        // 入口：返回单组件分布表（契约同旧 Solver::analyze，entries 按
        // mineCount 升序）。步骤：静态准备 → 首层表 → 逐层 transition →
        // 末层物化。不需要 graph：closed 与 remAfter 都由 shape + order
        // 静态推出，图只活在 order 产线里。
        Distribution analysis(const std::vector<BoxId>& order,
                              const Structure::Shape& shape);

    private:
        // 一层的状态表（SoA 平铺）。同层所有状态共享同一 layout（静态）。
        struct Layer {
            std::vector<long double> ways;     // [state]
            std::vector<long double> columns;  // [state * boxCount + box]
            std::vector<int> mineCounts;       // [state]
            std::vector<char> packed;          // [state * frontier + slot]
            FlatHashTable<U128, std::size_t, U128Hash> index;
            int boxCount = 0;  // columns 行宽（= 组件 box 数）
            int frontier = 0;  // packed 行宽（= 当前层 open box 数）

            std::size_t count() const { return ways.size(); }
        };

        // 内层检查一个约束所需的全部数据（外层逐层构造）。
        // 设 s = 该约束已赋值成员的和 = Σ_{m∈members} 槽[m]（v 未赋值，
        // 其槽为 0，故 members 给全体成员即可，无需区分已赋值子集）。
        // 对候选 k（本步给 v 的雷数），要求同时满足：
        //   s + k ≤ sum                   上界，超出即剪；
        //   s + k + remAfter ≥ sum        下界，后面成员全摆满也不够即剪；
        //   remAfter == 0 时上下界夹出唯一解 k = sum - s，即封口（v 是
        //   本约束最后一个被赋值的成员），k 偏离即该历史非法。
        // remAfter = 处理完 v 后本约束仍未赋值的成员 size 之和（不含
        // v），由成员在 order 中的位置静态求出。
        struct Check {
            const Structure::Shape::Constraint* constraint = nullptr;
            int remAfter = 0;  // 处理后剩余成员的 size 和，0 = 封口
        };

        // 单步转移的静态上下文（analysis 逐层构造，用完即弃）。
        struct Transition {
            BoxId v = 0;                             // 本步赋值的 box
            int boxSize = 0;
            const std::vector<BoxId>* closed = nullptr;  // 本步关闭的 box
            const std::vector<Check>* checks = nullptr;  // 本步检查的约束
            const std::vector<int>* slotOf = nullptr;    // box → 旧层槽位（-1 = 不在）
            const std::vector<int>* gather = nullptr;    // 新槽 d → 旧槽（-1 = v 新增）
            int newFrontier = 0;                         // 新层 open box 数
        };

        // 单层转移：before（层 k 表）→ after（层 k+1 表，写进线程局部
        // 池复用）。对每条状态：按 checks 收窄 k 的可行区间（封口定值后
        // 不提前退出，要过完 v 的全部约束再统一判区间），枚举可行 k；
        // 新状态 = 按 gather 拷贝旧 packed + v 槽写 k；
        // 新值 = 已关闭列 × C + 本层关闭列写入（x × 分支权重）+ ways × C；
        // 同键合并。closedSoFar = 本层之前已关闭的 box（列非零）。
        void transition(const Layer& before, Layer& after, const Transition& tr,
                        const std::vector<BoxId>& closedSoFar) const;
    };

    // 测试访问口：定义于 test/distribution/greedy_order.h。Graph 整体私有，
    // 探针经此友元才能命名并持有该类型。
    friend struct test::OrderProbe;

    // 组合数 C(n,k)：编译时查表。box 规模 ≤ 8（同一数字邻域集合的隐藏格
    // 都在该组任一数字的 8 邻域内）。
    static long double binom(int n, int k);
};


inline const DistributionSolver::Distribution* DistributionSolver::DistPool::get(const Structure::Shape* shape) {
    if (const DistributionSolver::Distribution** found = index_.find(shape->hash)) return *found;
    return nullptr;
}

inline const DistributionSolver::Distribution* DistributionSolver::DistPool::get(const Structure::Shape* shape) const {
    if (const DistributionSolver::Distribution* const* found = index_.find(shape->hash)) return *found;
    return nullptr;
}

inline const DistributionSolver::Distribution* DistributionSolver::DistPool::insert(const Structure::Shape* shape,
    Distribution dist) {
    if (const DistributionSolver::Distribution* found = get(shape)) return found;
    dists_.push_back(std::make_unique<DistributionSolver::Distribution>(std::move(dist)));
    index_.emplace(shape->hash, dists_.back().get());
    return dists_.back().get();
}

// ── Solver 私有工具实现 ──

inline long double DistributionSolver::binom(int n, int k) {
    constexpr int kMax = 9;
    // 编译期查表：静态 constexpr 表（lambda 初始化）。
    static constexpr std::array<std::array<long double, kMax + 1>, kMax + 1> kComb = []() {
        std::array<std::array<long double, kMax + 1>, kMax + 1> t{};
        for (int n = 0; n <= kMax; ++n) {
            t[n][0] = 1;
            t[n][n] = 1;
            for (int k = 1; k < n; ++k)
                t[n][k] =
                t[n - 1][k - 1] +
                t[n - 1][k];
        }
        return t;
        }();

    // 调用方保证 0<=k<=n<=box.size<=kMax；越界=结构 bug。
    assert_(k >= 0 && k <= n && n <= kMax, "Distribution:binom: 参数越界");
#if defined(_MSC_VER) && defined(_PREFAST_)
    // /analyze 无法从 assert_（普通函数）推断数组下界，这里显式告知
    // 分析器边界成立（仅代码分析时生效，不生成任何代码）。
    __analysis_assume(k >= 0 && k <= n && n <= kMax);
#endif
    return kComb[n][k];
}

inline void DistributionSolver::dpHelper::transition(
    const Layer& before, Layer& after, const Transition& tr,
    const std::vector<BoxId>& closedSoFar) const {
    // 复用 after 的容量（清除不释放）：层间与跨 analyze 调用都避免重分配。
    after.ways.clear();
    after.mineCounts.clear();
    after.packed.clear();
    after.columns.clear();
    after.index.clear();
    after.boxCount = before.boxCount;
    after.frontier = tr.newFrontier;

    const int boxCount = after.boxCount;
    const int oldFrontier = before.frontier;
    const int newFrontier = after.frontier;

    const std::size_t estimate =
        before.count() * static_cast<std::size_t>(tr.boxSize + 1);
    after.ways.reserve(estimate);
    after.mineCounts.reserve(estimate);
    after.packed.reserve(estimate * static_cast<std::size_t>(newFrontier));
    after.columns.reserve(estimate * static_cast<std::size_t>(boxCount));
    after.index.reserve(estimate);

    // 每转移的临时行：线程局部复用（只增长不释放），避免逐状态堆分配。
    static thread_local std::vector<char> packedRow;
    static thread_local std::vector<long double> colRow;
    if (packedRow.size() < static_cast<std::size_t>(newFrontier))
        packedRow.resize(static_cast<std::size_t>(newFrontier));
    if (colRow.size() < static_cast<std::size_t>(boxCount))
        colRow.resize(static_cast<std::size_t>(boxCount));

    const std::vector<BoxId>& closed = *tr.closed;
    const std::vector<Check>& checks = *tr.checks;
    const std::vector<int>& slotOf = *tr.slotOf;
    const std::vector<int>& gather = *tr.gather;

    for (std::size_t s = 0; s < before.count(); ++s) {
        const long double ways = before.ways[s];
        const int t = before.mineCounts[s];
        const char* packedSrc =
            before.packed.data() + s * static_cast<std::size_t>(oldFrontier);
        const long double* colSrc =
            before.columns.data() + s * static_cast<std::size_t>(boxCount);

        // 按 checks 收窄 k 的可行区间（封口定值后不提前退出，统一判）。
        // 成员的槽在旧层 slotOf 里：未赋值/已关闭 = -1 → 贡献 0。
        int minMine = 0;
        int maxMine = tr.boxSize;
        for (const Check& check : checks) {
            int partial = 0;
            for (BoxId member : check.constraint->boxIds) {
                const int slot = slotOf[static_cast<std::size_t>(member)];
                if (slot >= 0) partial += packedSrc[static_cast<std::size_t>(slot)];
            }
            minMine = std::max(minMine, check.constraint->sum - partial - check.remAfter);
            maxMine = std::min(maxMine, check.constraint->sum - partial);
        }

        for (int k = minMine; k <= maxMine; ++k) {
            const long double factor = DistributionSolver::binom(tr.boxSize, k);
            const long double waysNext = ways * factor;

            // 新状态 packed：按 gather 拷旧槽，v 新槽写 k（gather = -1）。
            for (int d = 0; d < newFrontier; ++d) {
                const int src = gather[static_cast<std::size_t>(d)];
                packedRow[static_cast<std::size_t>(d)] =
                    src < 0 ? static_cast<char>(k)
                            : packedSrc[static_cast<std::size_t>(src)];
            }

            // 新列：清零 → 已关闭列 × factor → 本层关闭列写入 x × waysNext。
            // （未赋值/未关闭列恒 0，缩放与合并只走非零列。）
            std::fill(colRow.begin(), colRow.begin() + boxCount, 0.0L);
            for (BoxId b : closedSoFar)
                colRow[static_cast<std::size_t>(b)] =
                    colSrc[static_cast<std::size_t>(b)] * factor;
            for (BoxId b : closed) {
                const long double xb =
                    b == tr.v   // v 本层赋值即关闭（不在旧层，值 = k）
                        ? static_cast<long double>(k)
                        : static_cast<long double>(packedSrc[static_cast<std::size_t>(
                              slotOf[static_cast<std::size_t>(b)])]);
                colRow[static_cast<std::size_t>(b)] += xb * waysNext;
            }

            // 合并键 = mineCount + packed（关闭/未赋值槽不参与，等价旧全向量判据）。
            U128Hasher hasher;
            hasher.mix(t + k);
            for (int d = 0; d < newFrontier; ++d)
                hasher.mix(static_cast<std::uint64_t>(
                    static_cast<unsigned char>(packedRow[static_cast<std::size_t>(d)])));
            const U128 hash = hasher.finalize();

            if (const std::size_t* found = after.index.find(hash)) {
                const std::size_t target = *found;
                after.ways[target] += waysNext;
                long double* colTarget =
                    after.columns.data() + target * static_cast<std::size_t>(boxCount);
                for (BoxId b : closedSoFar)
                    colTarget[static_cast<std::size_t>(b)] += colRow[static_cast<std::size_t>(b)];
                for (BoxId b : closed)
                    colTarget[static_cast<std::size_t>(b)] += colRow[static_cast<std::size_t>(b)];
            } else {
                const std::size_t id = after.count();
                after.ways.push_back(waysNext);
                after.mineCounts.push_back(t + k);
                after.packed.insert(after.packed.end(), packedRow.begin(),
                                    packedRow.begin() + newFrontier);
                after.columns.insert(after.columns.end(), colRow.begin(),
                                     colRow.begin() + boxCount);
                after.index.emplace(hash, id);
            }
        }
    }
}

inline DistributionSolver::Distribution DistributionSolver::dpHelper::analysis(
    const std::vector<BoxId>& order, const Structure::Shape& shape) {
    const int boxCount = static_cast<int>(shape.boxes.size());
    std::vector<int> position(boxCount);
    for (int step = 0; step < boxCount; ++step)
        position[order[static_cast<std::size_t>(step)]] = step;

    std::vector<int> closeStep = position;
    std::vector<std::vector<Check>> checksByStep(boxCount);
    for (const Structure::Shape::Constraint& constraint : shape.constraints) {
        int lastStep = -1;
        for (BoxId box : constraint.boxIds)
            lastStep = std::max(lastStep, position[static_cast<std::size_t>(box)]);
        for (BoxId box : constraint.boxIds)
            closeStep[static_cast<std::size_t>(box)] =
                std::max(closeStep[static_cast<std::size_t>(box)], lastStep);

        for (BoxId box : constraint.boxIds) {
            Check check;
            check.constraint = &constraint;
            for (BoxId member : constraint.boxIds)
                if (position[static_cast<std::size_t>(member)] >
                    position[static_cast<std::size_t>(box)])
                    check.remAfter += shape.boxes[static_cast<std::size_t>(member)].size;
            checksByStep[static_cast<std::size_t>(position[static_cast<std::size_t>(box)])]
                .push_back(std::move(check));
        }
    }

    std::vector<std::vector<BoxId>> closedByStep(boxCount);
    for (BoxId box = 0; box < boxCount; ++box)
        closedByStep[static_cast<std::size_t>(closeStep[static_cast<std::size_t>(box)])]
            .push_back(box);

    // 流式布局：open 集（order-position 升序）+ box→槽位映射 + 转移 gather。
    // 每层布局由"上一布局 - 本层关闭 + 追加 v"静态推出，DP 内无映射维护。
    std::vector<BoxId> layout;
    std::vector<int> slotOf(boxCount, -1);
    std::vector<char> closeStamp(boxCount, 0);
    std::vector<int> gather;
    std::vector<BoxId> nextLayout;
    std::vector<BoxId> closedSoFar;
    layout.reserve(boxCount);
    gather.reserve(boxCount);
    nextLayout.reserve(boxCount);
    closedSoFar.reserve(boxCount);

    // 线程局部双层池：cur/next 交替，跨 analyze 调用复用容量。
    static thread_local Layer tlCur;
    static thread_local Layer tlNext;
    Layer* cur = &tlCur;
    Layer* next = &tlNext;

    // 初始层：单状态（t=0，ways=1，packed 空，列全 0）。
    cur->ways.assign(1, 1.0L);
    cur->mineCounts.assign(1, 0);
    cur->packed.clear();
    cur->columns.assign(static_cast<std::size_t>(boxCount), 0.0L);
    cur->boxCount = boxCount;
    cur->frontier = 0;
    cur->index.clear();

    for (int step = 0; step < boxCount; ++step) {
        const BoxId v = order[static_cast<std::size_t>(step)];

        Transition tr;
        tr.v = v;
        tr.boxSize = shape.boxes[static_cast<std::size_t>(v)].size;
        tr.closed = &closedByStep[static_cast<std::size_t>(step)];
        tr.checks = &checksByStep[static_cast<std::size_t>(step)];
        tr.slotOf = &slotOf;

        // 本层关闭标记（stamp 免清零）→ 幸存者过滤；v 按 order-position
        // 恒追加在末尾（closeStep[v] == step 时本层赋值即关闭，不追加）。
        for (BoxId b : closedByStep[static_cast<std::size_t>(step)])
            closeStamp[static_cast<std::size_t>(b)] = 1;
        gather.clear();
        nextLayout.clear();
        for (std::size_t i = 0; i < layout.size(); ++i) {
            const BoxId b = layout[i];
            if (closeStamp[static_cast<std::size_t>(b)]) continue;
            gather.push_back(static_cast<int>(i));
            nextLayout.push_back(b);
        }
        if (closeStep[static_cast<std::size_t>(v)] > step) {
            gather.push_back(-1);  // v 新槽
            nextLayout.push_back(v);
        }
        tr.gather = &gather;
        tr.newFrontier = static_cast<int>(nextLayout.size());

        transition(*cur, *next, tr, closedSoFar);

        std::swap(cur, next);
        std::fill(slotOf.begin(), slotOf.end(), -1);
        for (int d = 0; d < static_cast<int>(nextLayout.size()); ++d)
            slotOf[static_cast<std::size_t>(nextLayout[static_cast<std::size_t>(d)])] = d;
        layout.swap(nextLayout);
        for (BoxId b : closedByStep[static_cast<std::size_t>(step)])
            closeStamp[static_cast<std::size_t>(b)] = 0;
        closedSoFar.insert(closedSoFar.end(),
                           closedByStep[static_cast<std::size_t>(step)].begin(),
                           closedByStep[static_cast<std::size_t>(step)].end());
    }

    // 末层物化：按 mineCount 升序出 entries（列 ÷ ways = perBoxExpectation）。
    const std::size_t stateCount = cur->count();
    std::vector<std::size_t> orderIdx(stateCount);
    for (std::size_t i = 0; i < stateCount; ++i) orderIdx[i] = i;
    std::sort(orderIdx.begin(), orderIdx.end(),
              [&](std::size_t a, std::size_t b) {
                  return cur->mineCounts[a] < cur->mineCounts[b];
              });

    Distribution result;
    for (std::size_t row : orderIdx) {
        Distribution::Entry entry;
        entry.mineCount = cur->mineCounts[row];
        entry.ways = cur->ways[row];
        entry.perBoxExpectation.resize(boxCount);
        const long double* col =
            cur->columns.data() + row * static_cast<std::size_t>(boxCount);
        for (int box = 0; box < boxCount; ++box)
            entry.perBoxExpectation[box] =
                col[static_cast<std::size_t>(box)] / entry.ways;
        result.entries.push_back(std::move(entry));
    }
    return result;
}

inline BoxId DistributionSolver::Graph::diameterStart() const {
    auto farthest = [this](BoxId start) {
        std::vector<char> visited(neighbors.size(), 0);
        BoxId farthestNode = start;
        int farthestDistance = 0;

        auto dfs = [&](auto&& self, BoxId node, int distance) -> void {
            visited[node] = 1;
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthestNode = node;
            }
            for (const BoxId neighbor : neighbors[node])
                if (!visited[neighbor])
                    self(self, neighbor, distance + 1);
        };

        dfs(dfs, start, 0);
        return farthestNode;
    };

    return farthest(0);
}

inline std::vector<BoxId> DistributionSolver::Graph::lexBfsOrder(BoxId init) const {
    const int n = static_cast<int>(neighbors.size());

    struct Cell {
        std::vector<BoxId> vertices;
        int orderIndex = -1;
        int stamp = -1;
        bool active = true;
    };

    std::vector<Cell> cells(1);
    cells[0].vertices.reserve(n);
    cells[0].orderIndex = 0;
    std::vector<int> cellOrder(1, 0);
    std::vector<int> cellOf(n, 0);
    std::vector<int> position(n);
    std::vector<int> splitCell(1, 0);
    std::vector<char> colored(n, false);
    std::vector<BoxId> order;
    std::vector<int> touched;
    order.reserve(n);
    touched.reserve(n);

    for (BoxId v = 0; v < n; ++v) {
        position[v] = static_cast<int>(cells[0].vertices.size());
        cells[0].vertices.push_back(v);
    }

    auto eraseVertex = [&](int cellId, BoxId v) {
        std::vector<BoxId>& vertices = cells[cellId].vertices;
        const int index = position[v];
        const BoxId moved = vertices.back();
        vertices[index] = moved;
        position[moved] = index;
        vertices.pop_back();
    };

    auto insertCellBefore = [&](int cellId) {
        const int orderIndex = cells[cellId].orderIndex;
        const int newCell = static_cast<int>(cells.size());
        cells.push_back(Cell{});
        splitCell.push_back(newCell);
        cellOrder.insert(cellOrder.begin() + orderIndex, newCell);
        for (int i = orderIndex; i < static_cast<int>(cellOrder.size()); ++i)
            cells[cellOrder[i]].orderIndex = i;
        return newCell;
    };

    int firstCell = 0;
    for (int step = 0; step < n; ++step) {
        while (!cells[cellOrder[firstCell]].active) ++firstCell;
        const int selectedCell = cellOrder[firstCell];
        BoxId v;

        if (step == 0) {
            v = init;
        } else {
            v = cells[selectedCell].vertices.front();
        }

        eraseVertex(selectedCell, v);
        colored[v] = true;
        order.push_back(v);
        touched.clear();

        for (const BoxId u : neighbors[v]) {
            if (colored[u]) continue;

            const int oldCell = cellOf[u];

            if (cells[oldCell].stamp != step) {
                const int newCell = insertCellBefore(oldCell);

                cells[oldCell].stamp = step;
                splitCell[oldCell] = newCell;
                touched.push_back(oldCell);
            }

            const int newCell = splitCell[oldCell];
            eraseVertex(oldCell, u);
            position[u] = static_cast<int>(cells[newCell].vertices.size());
            cells[newCell].vertices.push_back(u);
            cellOf[u] = newCell;
        }

        for (int oldCell : touched)
            if (cells[oldCell].vertices.empty()) cells[oldCell].active = false;
        if (cells[selectedCell].vertices.empty()) cells[selectedCell].active = false;
    }

    return order;
}

inline std::vector<BoxId> DistributionSolver::Graph::polishAdjacent(std::vector<BoxId> order,
                                                                    int rounds) const {
    const int n = static_cast<int>(order.size());

    for (int round = 0; round < rounds; ++round) {
        std::vector<int> rem(n);
        std::vector<char> colored(n, false);

        for (BoxId v = 0; v < n; ++v)
            rem[v] = static_cast<int>(neighbors[v].size());

        auto delta = [&](BoxId v) {
            int closed = 0;

            for (const BoxId u : neighbors[v])
                if (colored[u] && rem[u] == 1) ++closed;

            return static_cast<int>(rem[v] != 0) - closed;
        };

        auto color = [&](BoxId v) {
            colored[v] = true;

            for (const BoxId u : neighbors[v]) --rem[u];
        };

        for (int i = 0; i + 1 < n; ++i) {
            const BoxId a = order[i];
            const BoxId b = order[i + 1];

            const bool bIsAvailableBeforeA =
                static_cast<int>(neighbors[b].size()) > rem[b];

            if (bIsAvailableBeforeA && delta(b) < delta(a))
                std::swap(order[i], order[i + 1]);

            color(order[i]);
        }

        color(order.back());
    }
    return order;
}

inline std::vector<BoxId> DistributionSolver::Graph::polishAdjacent(BoxId init) const {
    return polishAdjacent(lexBfsOrder(init));
}

inline std::vector<BoxId> DistributionSolver::Graph::polishWindow3(std::vector<BoxId> order) const {
    const int n = static_cast<int>(order.size());

    std::vector<int> rem(n);
    std::vector<char> colored(n, false);

    for (BoxId v = 0; v < n; ++v)
        rem[v] = static_cast<int>(neighbors[v].size());

    auto delta = [&](BoxId v) {
        int closed = 0;

        for (const BoxId u : neighbors[v])
            if (colored[u] && rem[u] == 1) ++closed;

        return static_cast<int>(rem[v] != 0) - closed;
    };

    auto color = [&](BoxId v) {
        colored[v] = true;

        for (const BoxId u : neighbors[v]) --rem[u];
    };

    auto uncolor = [&](BoxId v) {
        for (const BoxId u : neighbors[v]) ++rem[u];

        colored[v] = false;
    };

    int bad = delta(order[0]);
    color(order[0]);

    for (int i = 1; i < n; i += 3) {
        const int length = std::min(3, n - i);

        std::array<BoxId, 3> candidate{};
        for (int j = 0; j < length; ++j)
            candidate[j] = order[i + j];

        std::sort(candidate.begin(), candidate.begin() + length);

        std::array<BoxId, 3> best{};
        int bestPeak = INT_MAX;
        int bestArea = INT_MAX;

        do {
            int localBad = bad;
            int peak = bad;
            int area = 0;
            int applied = 0;
            bool valid = true;

            for (int j = 0; j < length; ++j) {
                const BoxId v = candidate[j];

                if (static_cast<int>(neighbors[v].size()) == rem[v]) {
                    valid = false;
                    break;
                }

                localBad += delta(v);
                peak = std::max(peak, localBad);
                area += localBad;

                color(v);
                ++applied;
            }

            for (int j = applied - 1; j >= 0; --j)
                uncolor(candidate[j]);

            if (valid && std::pair{peak, area} < std::pair{bestPeak, bestArea}) {
                bestPeak = peak;
                bestArea = area;
                best = candidate;
            }
        } while (std::next_permutation(candidate.begin(), candidate.begin() + length));

        for (int j = 0; j < length; ++j) {
            order[i + j] = best[j];
            bad += delta(best[j]);
            color(best[j]);
        }
    }
    return order;
}

inline std::vector<BoxId> DistributionSolver::Graph::polishWindow3(BoxId init) const {
    return polishWindow3(lexBfsOrder(init));
}



template <DistributionSolver::PolishKind polish>
inline const DistributionSolver::Distribution* DistributionSolver::analyze(const Structure::Shape& shape, DistributionSolver::DistPool& pool) {
    if (const Distribution* cached = pool.get(&shape)) return cached;

    Graph graph;
    graph.neighbors.resize(shape.boxes.size());
    static thread_local FlatHashTable<U128, char, U128Hash> edgeSet;
    edgeSet.clear();

        for (const Structure::Shape::Constraint& constraint : shape.constraints) {
            for (std::size_t i = 0; i < constraint.boxIds.size(); ++i) {
                const BoxId lhs = constraint.boxIds[i];
                for (std::size_t j = i + 1; j < constraint.boxIds.size(); ++j) {
                    const BoxId rhs = constraint.boxIds[j];
                    const BoxId first = std::min(lhs, rhs);
                    const BoxId second = std::max(lhs, rhs);
                    const U128 edgeKey{static_cast<std::uint64_t>(first),
                                       static_cast<std::uint64_t>(second)};
                    if (edgeSet.find(edgeKey)) continue;
                    edgeSet.emplace(edgeKey, 0);
                    graph.neighbors[lhs].push_back(rhs);
                    graph.neighbors[rhs].push_back(lhs);
                }
            }
        }

    // 抛光默认自带 lexBfsOrder(diameterStart()) 产线；抛光方式由模板参数在
    // 编译期选定。
    const BoxId init = graph.diameterStart();
    std::vector<BoxId> order;
    if constexpr (polish == PolishKind::Adjacent) {
        order = graph.polishAdjacent(init);
    } else {
        order = graph.polishWindow3(init);
    }

    dpHelper helper;
    return pool.insert(&shape, helper.analysis(order, shape));
}


}  // namespace mss
