#pragma once

#include <array>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// probability.h — 概率引擎的共享数据类。
//
// 只定义"查询视图"，不含任何算法：
//   - Result：精确引擎的生产物，小（按块），非 O(nm) 网格。
//   - ComponentResult：单连通块的小结果（每 BoxId 的雷概率）。
//   - mineProbability：getter，单格查询，只读。
//   - frontierCells：getter，前沿格筛选（雷概率 < 阈值），按块枚举
//     不物化整盘网格——比"全盘扫描 + 单格查询"快一个数量级。
//   - ObserveResult：observe 引擎的产物——点开某格后的结果分布。
//
// 引擎实现：
//   analysis/probability/exact.h → Exact（GF 多项式 + binomial）
// 填 Result / ObserveResult（boxProbs/来源），UI 无需区分引擎。
// 整盘网格物化（ProbabilityGrid）归 UI 适配器。
// ─────────────────────────────────────────────────────────────

struct Probability {
    static long double exactMineProbability(long double p);

    // 单连通块的小结果。
    struct ComponentResult {
        // 每单位格(BoxId)的雷概率，已按 boxSize 均摊为单格概率。
        // 前沿格直接 boxProbs[boxId] 取值，无需再算。
        std::vector<long double> boxProbs;
    };

    // 精确/近似共享的查询视图。小（按块），非 O(nm) 网格。
    struct Result {

        std::vector<ComponentResult> components;  // 下标 = ComponentId，与 Structure::Result 对齐
        long double tCellProbability = 0;         // Unknown 格雷密度（标量）
        long double candidates = 0;               // 候选方案数

        // getter：单格雷概率，只读。返回：
        //   Mine=1，Unknown=tCellProbability，Safe/已揭示数字=0，
        //   前沿格 = boxProbs[boxId]。
        long double mineProbability(CellId cell, const ObservedBoard& board,
                                    const Basic::Result& basic,
                                    const Structure::Result& structure) const;


        // 前沿格筛选的一条记录：坐标 + 雷概率（同盒同值，取 boxProbs）。
        struct FrontierCell {
            int x = 0;              // 1-based 行
            int y = 0;              // 1-based 列
            long double p = 0;      // 该格雷概率
        };

        // getter：返回雷概率严格小于 p 的全部前沿格（坐标 + 概率）。
        // 按块枚举（components.boxProbs，复用 mineProbability 的取数路径），
        // 不物化整盘网格；概率 ≥ p 的 box 整盒跳过。调用方自排序。
        std::vector<FrontierCell> frontierCells(const ObservedBoard& board,
                                                const Structure::Result& structure,
                                                long double p) const;
    };

    // 观察结果：点开格子 x 后的结果分布。
    // 打开一个未开格的结果只有两种：踩雷，或显示数字 0..8。
    // 未开格恒有：explosion + Σ digit = 1。
    struct ObserveResult {
        std::array<long double, 9> digit = {};  // q_k：显示数字 k=0..8 的概率
        long double explosion = 0;              // P(x 是雷)（踩雷概率）
    };
};

// ── 实现区 ──
inline long double Probability::exactMineProbability(long double p) {
    return p >= 1.0L - 1e-10L ? 1.0L : p;
}

inline long double Probability::Result::mineProbability(
    CellId cell, const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure) const {
    const auto [x, y] = board.pos(cell);
    const CellLocation loc = structure.cellLoc[static_cast<std::size_t>(cell)];
    if (loc.component == -1) {
        if (basic.marks[x][y] == Basic::Mark::Mine) return 1.0L;
        if (basic.marks[x][y] == Basic::Mark::Unknown) return tCellProbability;
        return 0.0L;  // Safe / 已揭示数字
    }
    if (loc.box == -1) return 0.0L;  // 约束数字格（已揭示）
    return Probability::exactMineProbability(components[static_cast<std::size_t>(loc.component)].boxProbs[
        static_cast<std::size_t>(loc.box)]);
}

inline std::vector<Probability::Result::FrontierCell> Probability::Result::frontierCells(
    const ObservedBoard& board, const Structure::Result& structure, long double p) const {
    std::vector<FrontierCell> out;
    // 按块枚举：components 下标 = ComponentId，与 structure.components 对齐；
    // boxProbs 的取值路径与 mineProbability 相同（均摊单格概率）。
    for (std::size_t cid = 0; cid < components.size(); ++cid) {
        const ComponentResult& cr = components[static_cast<std::size_t>(cid)];
        const Structure::Instance& inst =
            structure.components[static_cast<std::size_t>(cid)];
        for (std::size_t b = 0; b < cr.boxProbs.size(); ++b) {
            const long double prob = cr.boxProbs[static_cast<std::size_t>(b)];
            if (!(prob < p)) continue;  // 整个 box 概率 ≥ p：整盒跳过
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [x, y] = board.pos(inst.boxes.cells[k]);  // 同 mineProbability 反解
                out.push_back(FrontierCell{x, y, prob});
            }
        }
    }
    return out;
}

}  // namespace mss
