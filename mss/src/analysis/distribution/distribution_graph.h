#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <iostream>
#include <list>
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

    // ── 前沿 DP：定长槽位，槽 = boxId ──
    //
    // 干什么：求一个组件的分布表 Z[t] / M[b][t]，替代暴力枚举。逐层沿
    // order 展开，每层维护一张状态表，一个状态 = 一个可合并的部分历史，
    // 值 = 该历史累计到当前的摆法权重。把"枚举全部 x 组合"换成"逐层
    // 枚举 + 状态合并"，复杂度由同层状态数（前沿宽度）主导。
    //
    // 状态长什么样：mines 定长 = box 数量，下标就是 boxId，无任何映射。
    // 每个槽的取值只有两类语义：
    //   0     = 未赋值，或已赋值但已关闭（不再被任何约束读取）；
    //   非 0  = 已赋值且仍在前沿，值就是雷数 x。
    // 约束检查对成员槽直接求和，未赋值/已关闭都贡献 0，不需要"已赋值
    // 子集"这类翻译。
    //
    // 合并判据：mineCount（累计雷数 t）+ mines 整体相等。完整判据本可以
    // 只比较"各活跃约束的部分和"（未来只读它）；用整体相等是更保守的
    // 充分条件，状态数可能偏多，但实现最简单，先保正确。
    //
    // 封口与关闭都是静态的，由 order 位置推出：
    //   封口：约束 c 的成员在 order 中的最晚位置 lastPos，层 k == lastPos
    //         时 c 封口，唯一合法 k 被夹死（见 Check::remAfter）；
    //   关闭：box b 的关闭层 closeStep = b 所有约束 lastPos 的最大值，
    //         因为 b 需要保留 ⟺ 它还有约束未封口。
    //
    // M 通道：mineCounts 定长 = box 数量，列下标即 boxId（不需要关闭
    // 顺序）。列 = Σ(ways × x)：box 关闭那一层写入 x × 分支权重，之后
    // 随分支缩放，末层 ÷ ways 得 perBoxExpectation。
    class dpHelper {
    public:
        // 入口：返回单组件分布表（契约同旧 Solver::analyze，entries 按
        // mineCount 升序）。步骤：静态准备 → 首层表 → 逐层 newBox →
        // 末层物化。不需要 graph：closed 与 remAfter 都由 shape + order
        // 静态推出，图只活在 order 产线里。
        Distribution analysis(const std::vector<BoxId>& order,
                              const Structure::Shape& shape);

    private:
        // 一层的状态表。
        struct Layer {
            // 状态 = 一个可合并的部分历史，见类注释。
            struct DpState {
                int mineCount = 0;        // 已赋值 box 的雷数总和，末层即 t
                std::vector<char> mines;  // 定长 n，槽 = boxId
                U128 hash = {};

                void updateHash();
            };

            // ways：该状态累计的摆法权重（已赋值 box 的组合数乘积）。
            // mineCounts[b]：定长 n，列 = boxId，box b 的 Σ(ways × x)，
            // 关闭层写入，末层 ÷ ways 即 perBoxExpectation。
            struct DpValue {
                long double ways = 0;
                std::vector<long double> mineCounts;
            };

            struct StateValue {
                DpState state;
                DpValue value;
            };

            // FlatHashTable 只提供查找/插入，不提供遍历。状态与统计量连续保存，
            // hash → 下标只作合并索引，避免哈希表 rehash 深拷贝 DpValue。
            FlatHashTable<U128, std::size_t, U128Hash> index;
            std::vector<StateValue> states;
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

        // 单层转移：before（层 k 表）→ 新表（层 k+1）。
        // 对每条状态：按 checks 收窄 k 的可行区间（封口定值后不提前
        // 退出，要过完 v 的全部约束再统一判区间），枚举可行 k；
        // 新状态 = 旧 mines 拷贝 + v 槽写 k + closed 槽清零；
        // 新值 = 旧列 × C + 本层关闭列写入（x × 分支权重）+ ways × C；
        // 同键合并。
        Layer newBox(const Layer& before, BoxId v, int boxSize,
                     const std::vector<BoxId>& closed,
                     const std::vector<Check>& checks) const;
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

inline void DistributionSolver::dpHelper::Layer::DpState::updateHash() {
    U128Hasher hasher;
    hasher.mix(mineCount);
    for (char mine : mines)
        hasher.mix(static_cast<std::uint64_t>(static_cast<unsigned char>(mine)));
    hash = hasher.finalize();
}

inline DistributionSolver::dpHelper::Layer DistributionSolver::dpHelper::newBox(
    const Layer& before, BoxId v, int boxSize, const std::vector<BoxId>& closed,
    const std::vector<Check>& checks) const {
    Layer after;
    after.index.reserve(before.states.size() * static_cast<std::size_t>(boxSize + 1));
    after.states.reserve(before.states.size() * static_cast<std::size_t>(boxSize + 1));

    for (const Layer::StateValue& current : before.states) {
        const Layer::DpState& state = current.state;
        const Layer::DpValue& value = current.value;

        int minMine = 0;
        int maxMine = boxSize;
        for (const Check& check : checks) {
            int partial = 0;
            for (BoxId member : check.constraint->boxIds)
                partial += state.mines[member];
            minMine = std::max(minMine, check.constraint->sum - partial - check.remAfter);
            maxMine = std::min(maxMine, check.constraint->sum - partial);
        }

        for (int mine = minMine; mine <= maxMine; ++mine) {
            Layer::DpState next = state;
            next.mineCount += mine;
            next.mines[v] = static_cast<char>(mine);

            const long double factor = DistributionSolver::binom(boxSize, mine);
            Layer::DpValue nextValue;
            nextValue.ways = value.ways * factor;
            nextValue.mineCounts.resize(next.mines.size());
            for (std::size_t b = 0; b < next.mines.size(); ++b)
                nextValue.mineCounts[b] = value.mineCounts[b] * factor;

            for (BoxId box : closed) {
                nextValue.mineCounts[box] += next.mines[box] * nextValue.ways;
                next.mines[box] = 0;
            }
            next.updateHash();

            if (const std::size_t* found = after.index.find(next.hash)) {
                Layer::DpValue& merged = after.states[*found].value;
                merged.ways += nextValue.ways;
                for (std::size_t b = 0; b < merged.mineCounts.size(); ++b)
                    merged.mineCounts[b] += nextValue.mineCounts[b];
            } else {
                const std::size_t id = after.states.size();
                after.states.push_back({std::move(next), std::move(nextValue)});
                after.index.emplace(after.states.back().state.hash, id);
            }
        }
    }

    return after;
}

inline DistributionSolver::Distribution DistributionSolver::dpHelper::analysis(
    const std::vector<BoxId>& order, const Structure::Shape& shape) {
    const int boxCount = static_cast<int>(shape.boxes.size());
    std::vector<int> position(boxCount);
    for (int step = 0; step < boxCount; ++step)
        position[order[step]] = step;

    std::vector<int> closeStep = position;
    std::vector<std::vector<Check>> checksByStep(boxCount);
    for (const Structure::Shape::Constraint& constraint : shape.constraints) {
        int lastStep = -1;
        for (BoxId box : constraint.boxIds)
            lastStep = std::max(lastStep, position[box]);
        for (BoxId box : constraint.boxIds)
            closeStep[box] = std::max(closeStep[box], lastStep);

        for (BoxId box : constraint.boxIds) {
            Check check;
            check.constraint = &constraint;
            for (BoxId member : constraint.boxIds)
                if (position[member] > position[box])
                    check.remAfter += shape.boxes[member].size;
            checksByStep[position[box]].push_back(std::move(check));
        }
    }

    std::vector<std::vector<BoxId>> closedByStep(boxCount);
    for (BoxId box = 0; box < boxCount; ++box)
        closedByStep[closeStep[box]].push_back(box);

    Layer layer;
    Layer::DpState initial;
    initial.mines.assign(boxCount, 0);
    initial.updateHash();
    Layer::DpValue initialValue;
    initialValue.ways = 1.0L;
    initialValue.mineCounts.assign(boxCount, 0.0L);
    layer.states.push_back({std::move(initial), std::move(initialValue)});
    layer.index.emplace(layer.states.back().state.hash, 0);

    for (int step = 0; step < boxCount; ++step) {
        const BoxId box = order[step];
        layer = newBox(layer, box, shape.boxes[box].size, closedByStep[step],
                       checksByStep[step]);
    }

    std::sort(layer.states.begin(), layer.states.end(),
              [](const Layer::StateValue& lhs, const Layer::StateValue& rhs) {
                  return lhs.state.mineCount < rhs.state.mineCount;
              });

    Distribution result;
    for (const Layer::StateValue& stateValue : layer.states) {
        const Layer::DpState& state = stateValue.state;
        const Layer::DpValue& value = stateValue.value;
        Distribution::Entry entry;
        entry.mineCount = state.mineCount;
        entry.ways = value.ways;
        entry.perBoxExpectation.resize(boxCount);
        for (int box = 0; box < boxCount; ++box)
            entry.perBoxExpectation[box] = value.mineCounts[box] / value.ways;
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

    using VertexList = std::list<BoxId>;

    struct Cell {
        VertexList vertices;
        int id = -1;
        int stamp = -1;
    };

    using Cells = std::list<Cell>;
    using CellIt = Cells::iterator;
    using VertexIt = VertexList::iterator;

    Cells cells(1);
    CellIt first = cells.begin();
    first->id = 0;

    std::vector<CellIt> cellOf(n);
    std::vector<VertexIt> position(n);
    std::vector<CellIt> splitCell(1, first);
    std::vector<char> colored(n, false);
    std::vector<BoxId> order;
    order.reserve(n);

    for (BoxId v = 0; v < n; ++v) {
        first->vertices.push_back(v);
        cellOf[v] = first;
        position[v] = std::prev(first->vertices.end());
    }

    for (int step = 0; step < n; ++step) {
        CellIt selectedCell;
        BoxId v;

        if (step == 0) {
            v = init;
            selectedCell = cellOf[v];
        } else {
            selectedCell = cells.begin();
            v = selectedCell->vertices.front();
        }

        selectedCell->vertices.erase(position[v]);
        colored[v] = true;
        order.push_back(v);

        std::vector<CellIt> touched;

        for (const BoxId u : neighbors[v]) {
            if (colored[u]) continue;

            CellIt oldCell = cellOf[u];

            if (oldCell->stamp != step) {
                CellIt newCell = cells.insert(oldCell, Cell{});
                newCell->id = static_cast<int>(splitCell.size());
                splitCell.push_back(newCell);

                oldCell->stamp = step;
                splitCell[oldCell->id] = newCell;
                touched.push_back(oldCell);
            }

            CellIt newCell = splitCell[oldCell->id];
            newCell->vertices.splice(newCell->vertices.end(), oldCell->vertices, position[u]);
            cellOf[u] = newCell;
        }

        if (selectedCell->stamp != step && selectedCell->vertices.empty())
            cells.erase(selectedCell);

        for (CellIt oldCell : touched)
            if (oldCell->vertices.empty()) cells.erase(oldCell);
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
