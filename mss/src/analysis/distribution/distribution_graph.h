#pragma once

#include <algorithm>
#include <array>
#include <climits>
#include <iostream>
#include <memory>
#include <span>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
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
        // CSR 邻接表：box b 的邻居位于 adjacent[offsets[b], offsets[b + 1])。
        // 只分配两段连续存储，不为每个 box 单独分配 vector。
        std::vector<int> offsets;
        std::vector<BoxId> adjacent;

        static Graph fromShape(const Structure::Shape& shape);
        int boxCount() const { return static_cast<int>(offsets.size()) - 1; }
        std::span<const BoxId> neighbors(BoxId box) const {
            const int begin = offsets[box];
            const int end = offsets[box + 1];
            return {adjacent.data() + begin, static_cast<std::size_t>(end - begin)};
        }

        // 展开起点：从 box 0 出发的 DFS 最远端点（启发式；原"双扫"的第二段
        // 端点无人使用，已删）。测试探针取 init 用，公开。
        BoxId diameterStart() const;

        // 对外只暴露两个成品入口：默认先用 lexBfsOrder(diameterStart())
        // 取展开序再抛光。

        // 相邻交换抛光成品（默认 2 轮），写入调用方复用的 order 缓冲。
        void polishAdjacent(BoxId init, std::vector<BoxId>& order) const;

        // 3 窗口全排列抛光成品，写入调用方复用的 order 缓冲。
        void polishWindow3(BoxId init, std::vector<BoxId>& order) const;

    private:
        // LexBFS 展开序（起点经 diameterStart 选取）：只被默认抛光入口
        // 调用，不外露。
        void lexBfsOrder(BoxId init, std::vector<BoxId>& order) const;

        // 抛光核心（相邻交换 rounds 轮，默认 2 / 3 窗口全排列）：给定 order
        // 就地抛光后返回。只被上面的默认入口调用，不外露。
        void polishAdjacent(std::vector<BoxId>& order, int rounds = 2) const;
        void polishWindow3(std::vector<BoxId>& order) const;
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
    // M 通道：moments[state][box] = Σ(ways × x)。box 关闭的那一步写入
    // 自己的 moment；之后随路径权重缩放、与同 Key 状态相加。momentBoxes
    // 只列出已经关闭的 box，所以合并不扫描尚未写入的零列。
    //
    // 平铺与复用：StateTable 用平铺数组保存 Key、Value、frontier 与 moments；
    // 两层 thread_local 表交替复用跨调用容量，无逐状态 vector/堆分配。
    class dpHelper {
    public:
        // 入口：返回单组件分布表（契约同旧 Solver::analyze，entries 按
        // mineCount 升序）。步骤：静态准备 → 首层表 → 逐层 transition →
        // 末层物化。不需要 graph：closed 与 remAfter 都由 shape + order
        // 静态推出，图只活在 order 产线里。
        Distribution analysis(const std::vector<BoxId>& order,
                              const Structure::Shape& shape);

    private:
        // StepPlan 是与路径无关的单步程序：analysis 由 Shape + order 编译一
        // 步后立刻交给 StateTable 执行；运行期不再碰 Shape、layout 或
        // box→slot 映射。
        struct StepPlan {
            struct Check {
                int sum = 0;
                int remAfter = 0;
                // 一个数字格最多邻接八个 box；这里只保留本步之前、仍在
                // frontier 中的成员槽位。
                std::array<int, 8> readSlots{};
                int readSlotCount = 0;
            };
            struct Closing {
                BoxId box = 0;
                int oldSlot = -1;  // -1 表示本步赋值的 box
            };

            BoxId box = 0;
            int boxSize = 0;
            std::vector<Check> checks;
            std::vector<int> gather;       // 新 frontier 槽 → 旧槽，-1 = box 新槽
            std::vector<Closing> closings; // 本步结算 moments 的 box
        };

        struct StateKey {
            int mineCount = 0;
        };

        struct StateValue {
            long double ways = 0.0L;
        };

        // 一层 DP 表。同层状态共享 frontier 宽度；frontiers 与 moments 都按
        // 状态平铺。momentBoxes 只记录哪些列已经有值，因此合并不扫描零列。
        class StateTable {
        public:
            void reset(int boxCount);
            void advance(const StepPlan& plan, StateTable& next) const;

            std::vector<StateKey> keys;
            std::vector<StateValue> values;
            std::vector<char> frontiers;
            std::vector<long double> moments;
            std::vector<BoxId> momentBoxes;
            FlatHashTable<U128, std::size_t, U128Hash> index;
            int boxCount = 0;
            int frontierSize = 0;

        private:
            static U128 hashKey(int mineCount, const char* frontier, int frontierSize);

            // 归 next 所有，跨 advance 调用复用；它们不是 DP 状态。
            mutable std::vector<char> frontierWork;
            mutable std::vector<long double> momentWork;
        };
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

#if defined(_MSC_VER) && defined(_PREFAST_)
    // 调用方保证 0<=k<=n<=box.size<=kMax；仅向静态分析器给出这个契约。
    __analysis_assume(k >= 0 && k <= n && n <= kMax);
#endif
    return kComb[n][k];
}

inline U128 DistributionSolver::dpHelper::StateTable::hashKey(
    int mineCount, const char* frontier, int frontierSize) {
    U128Hasher hasher;
    hasher.mix(mineCount);
    for (int slot = 0; slot < frontierSize; ++slot)
        hasher.mix(static_cast<std::uint64_t>(
            static_cast<unsigned char>(frontier[slot])));
    return hasher.finalize();
}

inline void DistributionSolver::dpHelper::StateTable::reset(int initialBoxCount) {
    keys.clear();
    values.clear();
    frontiers.clear();
    moments.clear();
    momentBoxes.clear();
    index.clear();
    boxCount = initialBoxCount;
    frontierSize = 0;
    keys.push_back(StateKey{.mineCount = 0});
    values.push_back(StateValue{.ways = 1.0L});
    moments.assign(boxCount, 0.0L);
}

inline void DistributionSolver::dpHelper::StateTable::advance(
    const StepPlan& plan, StateTable& next) const {
    next.keys.clear();
    next.values.clear();
    next.frontiers.clear();
    next.moments.clear();
    next.index.clear();
    next.boxCount = boxCount;
    next.frontierSize = static_cast<int>(plan.gather.size());
    next.momentBoxes = momentBoxes;
    for (const StepPlan::Closing& closing : plan.closings)
        next.momentBoxes.push_back(closing.box);

    const std::size_t estimate = keys.size() * (plan.boxSize + 1);
    next.keys.reserve(estimate);
    next.values.reserve(estimate);
    next.frontiers.reserve(estimate * next.frontierSize);
    next.moments.reserve(estimate * boxCount);
    next.index.reserve(estimate);
    if (next.frontierWork.size() < next.frontierSize)
        next.frontierWork.resize(next.frontierSize);
    if (next.momentWork.size() < boxCount)
        next.momentWork.resize(boxCount);

    for (std::size_t state = 0; state < keys.size(); ++state) {
        const StateKey key = keys[state];
        const long double ways = values[state].ways;
        const char* frontier =
            frontiers.data() + state * frontierSize;
        const long double* moment =
            moments.data() + state * boxCount;

        int minMine = 0;
        int maxMine = plan.boxSize;
        for (const StepPlan::Check& check : plan.checks) {
            int partial = 0;
            for (int i = 0; i < check.readSlotCount; ++i)
                partial += frontier[check.readSlots[i]];
            minMine = std::max(minMine, check.sum - partial - check.remAfter);
            maxMine = std::min(maxMine, check.sum - partial);
        }

        for (int mine = minMine; mine <= maxMine; ++mine) {
            const long double factor = DistributionSolver::binom(plan.boxSize, mine);
            const long double nextWays = ways * factor;
            for (int slot = 0; slot < next.frontierSize; ++slot) {
                const int source = plan.gather[slot];
                next.frontierWork[slot] =
                    source < 0 ? static_cast<char>(mine)
                               : frontier[source];
            }
            std::fill(next.momentWork.begin(),
                      next.momentWork.begin() + boxCount, 0.0L);
            for (BoxId box : momentBoxes)
                next.momentWork[box] =
                    moment[box] * factor;
            for (const StepPlan::Closing& closing : plan.closings) {
                const long double boxMineCount = closing.oldSlot < 0
                    ? static_cast<long double>(mine)
                    : static_cast<long double>(frontier[closing.oldSlot]);
                next.momentWork[closing.box] +=
                    boxMineCount * nextWays;
            }

            const int nextMineCount = key.mineCount + mine;
            const U128 hash = hashKey(nextMineCount, next.frontierWork.data(), next.frontierSize);
            if (const std::size_t* found = next.index.find(hash)) {
                const std::size_t target = *found;
                next.values[target].ways += nextWays;
                long double* targetMoment =
                    next.moments.data() + target * boxCount;
                for (BoxId box : momentBoxes)
                    targetMoment[box] +=
                        next.momentWork[box];
                for (const StepPlan::Closing& closing : plan.closings)
                    targetMoment[closing.box] +=
                        next.momentWork[closing.box];
            } else {
                const std::size_t id = next.keys.size();
                next.keys.push_back(StateKey{.mineCount = nextMineCount});
                next.values.push_back(StateValue{.ways = nextWays});
                next.frontiers.insert(next.frontiers.end(), next.frontierWork.begin(),
                                      next.frontierWork.begin() + next.frontierSize);
                next.moments.insert(next.moments.end(), next.momentWork.begin(),
                                    next.momentWork.begin() + boxCount);
                next.index.emplace(hash, id);
            }
        }
    }
}

inline DistributionSolver::Distribution DistributionSolver::dpHelper::analysis(
    const std::vector<BoxId>& order, const Structure::Shape& shape) {
    const int boxCount = static_cast<int>(shape.boxes.size());
    static thread_local std::vector<int> position;
    position.resize(boxCount);
    for (int step = 0; step < boxCount; ++step)
        position[order[step]] = step;

    static thread_local std::vector<int> closeStep;
    closeStep = position;
    // 约束→box 关联用单条链表平铺，避免每个 box 各自分配一个小 vector。
    static thread_local std::vector<int> constraintHead;
    static thread_local std::vector<int> constraintNext;
    static thread_local std::vector<int> constraintIds;
    constraintHead.assign(boxCount, -1);
    constraintNext.clear();
    constraintIds.clear();
    std::size_t constraintIncidences = 0;
    for (const Structure::Shape::Constraint& constraint : shape.constraints)
        constraintIncidences += constraint.boxIds.size();
    constraintNext.reserve(constraintIncidences);
    constraintIds.reserve(constraintIncidences);
    for (int constraintId = 0; constraintId < static_cast<int>(shape.constraints.size());
         ++constraintId) {
        const Structure::Shape::Constraint& constraint =
            shape.constraints[constraintId];
        int lastStep = -1;
        for (BoxId box : constraint.boxIds)
            lastStep = std::max(lastStep, position[box]);
        for (BoxId box : constraint.boxIds) {
            closeStep[box] =
                std::max(closeStep[box], lastStep);
            const std::size_t index = constraintIds.size();
            constraintNext.push_back(constraintHead[box]);
            constraintIds.push_back(constraintId);
            constraintHead[box] = static_cast<int>(index);
        }
    }

    static thread_local std::vector<BoxId> closeHead;
    static thread_local std::vector<BoxId> closeNext;
    closeHead.assign(boxCount, -1);
    closeNext.assign(boxCount, -1);
    for (BoxId box = 0; box < boxCount; ++box) {
        const int step = closeStep[box];
        closeNext[box] = closeHead[step];
        closeHead[step] = box;
    }

    // 编译步骤计划：运行时不再构造 layout 或查询 constraint->boxIds。
    static thread_local std::vector<BoxId> layout;
    static thread_local std::vector<int> slotOf;
    static thread_local std::vector<char> closeStamp;
    static thread_local std::vector<BoxId> nextLayout;
    layout.clear();
    slotOf.assign(boxCount, -1);
    closeStamp.assign(boxCount, 0);
    nextLayout.clear();
    layout.reserve(boxCount);
    nextLayout.reserve(boxCount);

    // 线程局部双层表：StateTable 的容量跨 analyze 调用复用。
    static thread_local StateTable tlCur;
    static thread_local StateTable tlNext;
    StateTable* cur = &tlCur;
    StateTable* next = &tlNext;
    cur->reset(boxCount);

    // StepPlan 只在编译完成的那一刻被消费；复用它的三个缓冲区，避免每个
    // component 为每一步各自分配一组 vector。
    static thread_local StepPlan plan;
    for (int step = 0; step < boxCount; ++step) {
        plan.checks.clear();
        plan.gather.clear();
        plan.closings.clear();
        plan.box = order[step];
        plan.boxSize = shape.boxes[plan.box].size;
        for (int index = constraintHead[plan.box]; index >= 0;
             index = constraintNext[index]) {
            const Structure::Shape::Constraint& constraint =
                shape.constraints[constraintIds[index]];
            StepPlan::Check check;
            check.sum = constraint.sum;
            for (BoxId member : constraint.boxIds) {
                const int memberStep = position[member];
                if (memberStep < step)
                    check.readSlots[check.readSlotCount++] =
                        slotOf[member];
                else if (memberStep > step)
                    check.remAfter += shape.boxes[member].size;
            }
            plan.checks.push_back(check);
        }

        for (BoxId box = closeHead[step]; box >= 0;
             box = closeNext[box])
            closeStamp[box] = 1;
        nextLayout.clear();
        for (std::size_t oldSlot = 0; oldSlot < layout.size(); ++oldSlot) {
            const BoxId box = layout[oldSlot];
            if (closeStamp[box]) continue;
            plan.gather.push_back(static_cast<int>(oldSlot));
            nextLayout.push_back(box);
        }
        if (closeStep[plan.box] > step) {
            plan.gather.push_back(-1);
            nextLayout.push_back(plan.box);
        }
        for (BoxId box = closeHead[step]; box >= 0;
             box = closeNext[box])
            plan.closings.push_back(StepPlan::Closing{
                .box = box,
                .oldSlot = box == plan.box ? -1 : slotOf[box],
            });

        cur->advance(plan, *next);
        std::swap(cur, next);

        std::fill(slotOf.begin(), slotOf.end(), -1);
        for (int slot = 0; slot < static_cast<int>(nextLayout.size()); ++slot)
            slotOf[nextLayout[slot]] = slot;
        layout.swap(nextLayout);
        for (BoxId box = closeHead[step]; box >= 0;
             box = closeNext[box])
            closeStamp[box] = 0;
    }

    // 末层物化：按 mineCount 升序出 entries（列 ÷ ways = perBoxExpectation）。
    const std::size_t stateCount = cur->keys.size();
    Distribution result;
    result.entries.reserve(stateCount);
    for (std::size_t row = 0; row < stateCount; ++row) {
        Distribution::Entry entry;
        entry.mineCount = cur->keys[row].mineCount;
        entry.ways = cur->values[row].ways;
        entry.perBoxExpectation.resize(boxCount);
        const long double* moment =
            cur->moments.data() + row * boxCount;
        for (int box = 0; box < boxCount; ++box)
            entry.perBoxExpectation[box] =
                moment[box] / entry.ways;
        result.entries.push_back(std::move(entry));
    }
    std::sort(result.entries.begin(), result.entries.end(),
              [](const Distribution::Entry& lhs, const Distribution::Entry& rhs) {
                  return lhs.mineCount < rhs.mineCount;
              });
    return result;
}

inline DistributionSolver::Graph DistributionSolver::Graph::fromShape(
    const Structure::Shape& shape) {
    Graph graph;
    const int boxCount = static_cast<int>(shape.boxes.size());
    graph.offsets.assign(boxCount + 1, 0);

    // 先以链式前向星收集候选边。一个约束内的 box 对天然是双向边；跨约束
    // 的重复在转 CSR 时按源点用 marks 去掉，不需要 hash 表。
    static thread_local std::vector<int> head;
    static thread_local std::vector<BoxId> to;
    static thread_local std::vector<int> next;
    static thread_local std::vector<char> marked;
    head.assign(boxCount, -1);
    to.clear();
    next.clear();
    marked.assign(boxCount, false);

    auto addEdge = [&](BoxId from, BoxId target) {
        next.push_back(head[from]);
        to.push_back(target);
        head[from] = static_cast<int>(to.size()) - 1;
    };
    for (const Structure::Shape::Constraint& constraint : shape.constraints)
        for (std::size_t i = 0; i < constraint.boxIds.size(); ++i)
            for (std::size_t j = i + 1; j < constraint.boxIds.size(); ++j) {
                const BoxId lhs = constraint.boxIds[i];
                const BoxId rhs = constraint.boxIds[j];
                addEdge(lhs, rhs);
                addEdge(rhs, lhs);
            }

    // 第一遍：按源点去重并统计 CSR 行宽；第二遍会沿同一链表回退清除标记。
    for (BoxId box = 0; box < boxCount; ++box) {
        for (int edge = head[box]; edge >= 0;
             edge = next[edge]) {
            const BoxId target = to[edge];
            if (marked[target]) continue;
            marked[target] = true;
            ++graph.offsets[box + 1];
        }
        for (int edge = head[box]; edge >= 0;
             edge = next[edge])
            marked[to[edge]] = false;
    }
    for (int box = 0; box < boxCount; ++box)
        graph.offsets[box + 1] +=
            graph.offsets[box];

    // 第二遍：每次只填当前 box 的连续区间，因此不需要 cursors 副本。
    graph.adjacent.resize(graph.offsets.back());
    for (BoxId box = 0; box < boxCount; ++box) {
        int write = graph.offsets[box];
        for (int edge = head[box]; edge >= 0;
             edge = next[edge]) {
            const BoxId target = to[edge];
            if (marked[target]) continue;
            marked[target] = true;
            graph.adjacent[write++] = target;
        }
        for (int edge = head[box]; edge >= 0;
             edge = next[edge])
            marked[to[edge]] = false;
    }
    return graph;
}

inline BoxId DistributionSolver::Graph::diameterStart() const {
    auto farthest = [this](BoxId start) {
        static thread_local std::vector<char> visited;
        visited.assign(boxCount(), 0);
        BoxId farthestNode = start;
        int farthestDistance = 0;

        auto dfs = [&](auto&& self, BoxId node, int distance) -> void {
            visited[node] = 1;
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthestNode = node;
            }
            for (const BoxId neighbor : neighbors(node))
                if (!visited[neighbor])
                    self(self, neighbor, distance + 1);
        };

        dfs(dfs, start, 0);
        return farthestNode;
    };

    return farthest(0);
}

inline void DistributionSolver::Graph::lexBfsOrder(BoxId init, std::vector<BoxId>& order) const {
    const int n = boxCount();

    struct Cell {
        std::vector<BoxId> vertices;
        int orderIndex = -1;
        int stamp = -1;
        bool active = true;
    };

    static thread_local std::vector<Cell> cells;
    static thread_local std::vector<int> cellOrder;
    static thread_local std::vector<int> cellOf;
    static thread_local std::vector<int> position;
    static thread_local std::vector<int> splitCell;
    static thread_local std::vector<char> colored;
    static thread_local std::vector<int> touched;
    if (cells.empty()) cells.emplace_back();
    Cell& first = cells[0];
    first.vertices.clear();
    first.orderIndex = 0;
    first.stamp = -1;
    first.active = true;
    int cellCount = 1;
    first.vertices.reserve(n);
    cellOrder.assign(1, 0);
    cellOf.assign(n, 0);
    position.resize(n);
    splitCell.assign(1, 0);
    colored.assign(n, false);
    order.clear();
    order.reserve(n);
    touched.clear();
    touched.reserve(n);

    for (BoxId v = 0; v < n; ++v) {
        position[v] = static_cast<int>(first.vertices.size());
        first.vertices.push_back(v);
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
        const int newCell = cellCount++;
        if (newCell == static_cast<int>(cells.size())) cells.emplace_back();
        Cell& cell = cells[newCell];
        cell.vertices.clear();
        cell.orderIndex = orderIndex;
        cell.stamp = -1;
        cell.active = true;
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

        for (const BoxId u : neighbors(v)) {
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

}

inline void DistributionSolver::Graph::polishAdjacent(std::vector<BoxId>& order,
                                                       int rounds) const {
    const int n = static_cast<int>(order.size());

    for (int round = 0; round < rounds; ++round) {
        static thread_local std::vector<int> rem;
        static thread_local std::vector<int> closingGain;
        static thread_local std::vector<char> colored;
        rem.resize(n);
        closingGain.assign(n, 0);
        colored.assign(n, false);

        for (BoxId v = 0; v < n; ++v)
            rem[v] = static_cast<int>(neighbors(v).size());

        auto delta = [&](BoxId v) {
            return static_cast<int>(rem[v] != 0) - closingGain[v];
        };

        auto color = [&](BoxId v) {
            colored[v] = true;

            // v 若只剩一个未选择邻居，就从此刻起为该邻居贡献一次关闭收益。
            if (rem[v] == 1)
                for (const BoxId u : neighbors(v))
                    if (!colored[u]) {
                        ++closingGain[u];
                        break;
                    }

            // 已选择邻居从 rem=2 变为 rem=1 时，也恰好出现唯一的未选择
            // 邻居；它从这一刻起贡献关闭收益。每个 box 最多经历一次 2→1，
            // 因而这些补充扫描总计 O(E)。
            for (const BoxId u : neighbors(v)) {
                const int oldRem = rem[u];
                --rem[u];
                if (colored[u] && oldRem == 2)
                    for (const BoxId w : neighbors(u))
                        if (!colored[w]) {
                            ++closingGain[w];
                            break;
                        }
            }
        };

        for (int i = 0; i + 1 < n; ++i) {
            const BoxId a = order[i];
            const BoxId b = order[i + 1];

            const bool bIsAvailableBeforeA =
                static_cast<int>(neighbors(b).size()) > rem[b];

            if (bIsAvailableBeforeA && delta(b) < delta(a))
                std::swap(order[i], order[i + 1]);

            color(order[i]);
        }

        color(order.back());
    }
}

inline void DistributionSolver::Graph::polishAdjacent(BoxId init, std::vector<BoxId>& order) const {
    lexBfsOrder(init, order);
    polishAdjacent(order);
}

inline void DistributionSolver::Graph::polishWindow3(std::vector<BoxId>& order) const {
    const int n = static_cast<int>(order.size());

    static thread_local std::vector<int> rem;
    static thread_local std::vector<char> colored;
    rem.resize(n);
    colored.assign(n, false);

    for (BoxId v = 0; v < n; ++v)
        rem[v] = static_cast<int>(neighbors(v).size());

    auto delta = [&](BoxId v) {
        int closed = 0;

        for (const BoxId u : neighbors(v))
            if (colored[u] && rem[u] == 1) ++closed;

        return static_cast<int>(rem[v] != 0) - closed;
    };

    auto color = [&](BoxId v) {
        colored[v] = true;

        for (const BoxId u : neighbors(v)) --rem[u];
    };

    auto uncolor = [&](BoxId v) {
        for (const BoxId u : neighbors(v)) ++rem[u];

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

                if (static_cast<int>(neighbors(v).size()) == rem[v]) {
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
}

inline void DistributionSolver::Graph::polishWindow3(BoxId init, std::vector<BoxId>& order) const {
    lexBfsOrder(init, order);
    polishWindow3(order);
}



template <DistributionSolver::PolishKind polish>
inline const DistributionSolver::Distribution* DistributionSolver::analyze(const Structure::Shape& shape, DistributionSolver::DistPool& pool) {
    if (const Distribution* cached = pool.get(&shape)) return cached;

    Graph graph = Graph::fromShape(shape);

    // 抛光默认自带 lexBfsOrder(diameterStart()) 产线；抛光方式由模板参数在
    // 编译期选定。
    const BoxId init = graph.diameterStart();
    static thread_local std::vector<BoxId> order;
    if constexpr (polish == PolishKind::Adjacent) {
        graph.polishAdjacent(init, order);
    } else {
        graph.polishWindow3(init, order);
    }

    dpHelper helper;
    return pool.insert(&shape, helper.analysis(order, shape));
}


}  // namespace mss
