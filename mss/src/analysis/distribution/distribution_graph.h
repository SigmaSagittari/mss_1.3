#pragma once

#include <algorithm>
#include <array>
#include <iostream>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

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
    // ShapePool 生命周期内稳定，跨盘面新建池时堆地址会被 malloc 复用，旧
    // 指针误中旧分布（曾导致 Exact::analyze 的 boxProbs 反转污染）。
    struct DistPool {
        const Distribution* get(const Structure::Shape* shape);
        const Distribution* get(const Structure::Shape* shape) const;
        const Distribution* insert(const Structure::Shape* shape, Distribution dist);

    private:
        std::vector<std::unique_ptr<Distribution>> dists_;  // 稳定地址，只增不删
        FlatHashTable<U128, const Distribution*, U128Hash> index_;
    };

    // 汇总分布表（forEachAssignment 聚合），经 DistPool 去重缓存。
    static const Distribution* analyze(const Structure::Shape& shape, DistPool& pool);

    struct Graph {
        // 仅按 Shape 的 BoxId 编号
        std::vector<std::vector<BoxId>> neighbors;
    };

    // 从 init 出发，逐个选择 box。每次从候选中选取使已选择 / 未选择
    // 分界上 box 数量变化最小的点；返回按选择先后排列的 BoxId。
    static std::vector<int> greedyOrder(const Graph& graph, BoxId init);

private:
    static std::pair<BoxId, BoxId> findDiameter(const Graph& graph);
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

    // 调用方（forEachAssignment）保证 0<=k<=n<=box.size<=kMax；越界=结构 bug。
    assert_(k >= 0 && k <= n && n <= kMax, "Distribution:binom: 参数越界");
    return kComb[n][k];
}

inline std::pair<BoxId, BoxId> DistributionSolver::findDiameter(const Graph& graph) {
    auto farthest = [&graph](BoxId start) {
        std::vector<char> visited(graph.neighbors.size(), 0);
        BoxId farthestNode = start;
        int farthestDistance = 0;

        auto dfs = [&](auto&& self, BoxId node, int distance) -> void {
            visited[node] = 1;
            if (distance > farthestDistance) {
                farthestDistance = distance;
                farthestNode = node;
            }
            for (const BoxId neighbor : graph.neighbors[node])
                if (!visited[neighbor])
                    self(self, neighbor, distance + 1);
        };

        dfs(dfs, start, 0);
        return farthestNode;
    };

    const BoxId first = farthest(0);
    const BoxId second = farthest(first);
    return {first, second};
}

inline std::vector<int> DistributionSolver::greedyOrder(const Graph& graph, BoxId init) {
    const int n = static_cast<int>(graph.neighbors.size());
    // selected：已放入顺序的 box。其余 box 均是未选择的 box。
    std::vector<char> selected(n, 0);
    // candidate：未选择、但至少连接一个已选择 box 的 box。
    std::vector<char> candidate(n, 0);
    // unselectedNeighbors[u]：u 当前还连接多少未选择 box。
    std::vector<int> unselectedNeighbors(n, 0);
    // closesSelected[u]：若下一步选择 u，能让多少已选择 box 彻底脱离边界。
    std::vector<int> closesSelected(n, 0);
    std::vector<int> score(n, 0);
    // 按 (score, BoxId) 排序；同分时稳定地选择较小 BoxId。
    std::set<std::pair<int, BoxId>> candidates;
    std::vector<int> order;
    order.reserve(n);
    for (int i = 0; i < n; ++i)
        unselectedNeighbors[i] = static_cast<int>(graph.neighbors[i].size());

    // 每轮都直接展开一个已选点周围的局部变化。这里没有“上下游”：只有
    // 已选择与未选择。一个已选择 box 只有在不存在未选择邻居时才离开边界。
    BoxId box = init;
    while (true) {
        if (candidate[box])
            candidates.erase({score[box], box});
        selected[box] = 1;
        candidate[box] = 0;
        order.push_back(box);

        for (BoxId neighbor : graph.neighbors[box]) {
            // box 刚从未选择变为已选择；每个邻居都少了一个未选择邻居。
            --unselectedNeighbors[neighbor];
            if (selected[neighbor]) {
                // neighbor 从“还连两个未选择点”变为“只连一个未选择点”。
                // 找到这个唯一的未选择点 last；选择 last 时便能关闭 neighbor。
                if (unselectedNeighbors[neighbor] == 1) {
                    BoxId last = static_cast<BoxId>(-1);
                    for (BoxId next : graph.neighbors[neighbor])
                        if (!selected[next]) {
                            last = next;
                            break;
                        }
                    candidates.erase({score[last], last});
                    ++closesSelected[last];
                    score[last] = (unselectedNeighbors[last] != 0) - closesSelected[last];
                    candidates.emplace(score[last], last);
                }
            } else {
                // neighbor 仍未选择；它现在已连接已选择部分，成为候选。
                if (candidate[neighbor])
                    candidates.erase({score[neighbor], neighbor});
                candidate[neighbor] = 1;
                score[neighbor] = (unselectedNeighbors[neighbor] != 0) - closesSelected[neighbor];
                candidates.emplace(score[neighbor], neighbor);
            }
        }

        // box 自己刚变为已选择。若它只剩一个未选择邻居，那个点日后会关闭 box。
        if (unselectedNeighbors[box] == 1) {
            BoxId last = static_cast<BoxId>(-1);
            for (BoxId next : graph.neighbors[box])
                if (!selected[next]) {
                    last = next;
                    break;
                }
            candidates.erase({score[last], last});
            ++closesSelected[last];
            score[last] = (unselectedNeighbors[last] != 0) - closesSelected[last];
            candidates.emplace(score[last], last);
        }
        if (static_cast<int>(order.size()) == n) break;
        box = candidates.begin()->second;
    }
    return order;
}

inline const DistributionSolver::Distribution* DistributionSolver::analyze(const Structure::Shape& shape, DistributionSolver::DistPool& pool) {
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

    const auto diameter = findDiameter(graph);
    const std::vector<int> order = greedyOrder(graph, diameter.first);

    std::cout << "distribution/greedy-order: boxes=" << shape.boxes.size()
              << ", diameter=" << diameter.first << '-' << diameter.second
              << "\n  order:";
    for (int box : order) std::cout << ' ' << box;
    std::cout << '\n';
    return pool.get(&shape);
}


}  // namespace mss
