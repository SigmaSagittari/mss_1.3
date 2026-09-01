#pragma once

#include <algorithm>
#include <array>
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
    private:
        // 组合数 C(n,k)：编译时查表。box 规模 ≤ 8（同一数字邻域集合的隐藏格
        // 都在该组任一数字的 8 邻域内）。
    static long double binom(int n, int k);
};

}  // namespace mss
