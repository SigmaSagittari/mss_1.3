#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// midgame/secondary_safety_evaluator.h — 中盘猜点：选"点开哪个格"。
//
// 用法：没有确定安全格时，一行
//     SecondarySafetyEvaluator::Result r = solve(board, basic, structure, prob, shapePool, pool);
//     得到推荐格 r.x, r.y（候选细节在 r.candidates）。
//
// 思路：对每个候选格，假装翻开它并评估新局面的"可玩性"，按概率加权打分：
//   打分 = Σ_v P(翻出 v) × 新局面"最好/次好安全度"混合 + "翻完还能继续开"
//   （progress）奖励——翻完后续好走的格优先点。
//
// 来源：Java 版 SecondarySafetyEvaluator（MineSweeperSolver）移植；
//   删去 50/50 相关部分；参数与行为一律以 Java 版为准（实现内保留 Java
//   方法名便于对照）。
// ─────────────────────────────────────────────────────────────

struct SecondarySafetyEvaluator {
    // 参数与 Java 版完全一致（SolverSettings.java 的默认值）。
    struct Config {
        // "翻完还能继续开"的加分权重。0.001 ≈ 几乎不加分，只作平分时的
        //  打破规则；调参实验才动。
        long double progressContribution = 0.001L;

        // 候选窗主门槛：安全度（= 1 − 雷概率）比最强的活格低 0.10 以内都收。
        //  是差值不是比例：最强安全度 0.95 → 安全度 ≥ 0.85 的格全进候选窗。
        long double selectionThreshold1 = 0.10L;

        // 候选窗兜底线：主门槛收进来不足 2 个候选时，放宽到低 0.20 以内
        //  （0.95 → ≥ 0.75）多收几个，保证至少 2 个候选可比；候选够多时不放宽。
        long double selectionThreshold2 = 0.20L;

        // 候选所在"未开连片区域"少于该格数 → 排后评估（点开大区域易翻 0、
        //  连锁展开，收益高）；全部候选都在小区域时才轮到它们。
        int spaceThreshold = 8;

        // "翻成数字 v 后的新局面"用什么数代表：最好的格 ×4 + 次好的格 ×1，
        //  除以 5。改它 = 改对"后续最优格"的信任配比，一般不动。
        int weight1 = 4;
        int weight2 = 1;
        Config() = default;
    };

    // 一次求解的产物。
    struct Result {
        // 单候选报告：评估细节 + 分数（供排序与复盘）。
        struct Candidate {
            int x = 0;                  // 1-based 行
            int y = 0;                  // 1-based 列
            long double safety = 0;     // 这格本身的安全度 P(安全)
            long double expectedClears = 0;   // 期望连带开出的格数（排序并列时使用）
            long double weight = 0;     // 最终分 = secondarySafety × (1 + cont×progressProb)
        };

        int x = 0;                      // 推荐点击格（1-based；无候选 = 0,0）
        int y = 0;
        long double weight = 0;         // 推荐格的分数
        std::vector<Candidate> candidates;  // 全部候选（按分数降序）
    };

    // 求解一次：候选窗 → 逐格评估 → 剪枝 → 返回分数最高的格。
    // prob 必须是同一局面的 Probability::Result；shapePool 供评估中临时
    // 揭示产生的新形状 intern（同 Structure::update）。
    // config 必传（Config{} 即全默认；带类内默认成员初始化器的类型不支持
    //   = Config{} 形式的默认参数，C++ 规则所致）。
    // 求解期间会通过 Delta 临时修改 board / basic / structure，并在返回前恢复。
    static Result solve(ObservedBoard& board, Basic::Result& basic,
                        Structure::Result& structure,
                        const Probability::Result& prob,
                        Structure::ShapePool& shapePool,
                        Distribution::DistPool& pool,
                        const Config& config);

private:
    // 单格评估（solve 的辅助；bestWeight = 当前最高分，供剪枝用；0 = 尚无第一名）。
    static Result::Candidate evaluate(ObservedBoard& board,
                                      Basic::Result& basic,
                                      Structure::Result& structure,
                                      const Probability::Result& prob,
                                      Structure::ShapePool& shapePool,
                                      Distribution::DistPool& pool, CellId cell,
                                      long double bestWeight,
                                      const Config& config);

    struct FrontierSummary {
        long double clears = 0;
        long double blendedSafety = 0;
    };

    // 一次前沿遍历同时统计确定安全格数和最优/次优安全度混合。
    static FrontierSummary summarizeFrontier(const Probability::Result& p,
                                             const ObservedBoard& board,
                                             const Structure::Result& structure,
                                             const Config& config);
};

// ── 实现区 ──
//
// evaluate（对齐 Java doFullEvaluateTile）：
//   1. observe 一次得到 x 的踩雷率和每个数字结果的概率。
//   2. 对每个正概率数字 v：临时把 x 揭示为 v，Basic/Structure 增量更新，
//      analyze 新局面，再 applyDelta(reverse=true) 撤销。
//   3. 汇总：secondarySafety = Σ prob_v × blendedSafety_v；
//      progressProb = Σ prob_v（该结果存在确定安全格）；
//      weight = secondarySafety × (1 + cont × progressProb)。
//   4. 剪枝：bestWeight 已足够高，且"已累计分数 + 剩余概率全按 100%
//      安全"仍追不上 → pruned = true 提前返回（weight 记 0，排序垫底）。

inline SecondarySafetyEvaluator::FrontierSummary SecondarySafetyEvaluator::summarizeFrontier(
    const Probability::Result& p, const ObservedBoard& board,
    const Structure::Result& s, const Config& config) {
    // 起点 = 离网安全度（Java offEdgeSafety 同款）。
    FrontierSummary out;
    long double best = 1.0L - p.tCellProbability;
    long double second = best;
    p.frontierCells(board, s, [&](int, int, long double probability) {
        if (probability == 0.0L) ++out.clears;
        const long double safety = 1.0L - probability;
        if (safety > best) {
            second = best;
            best = safety;
        } else if (safety > second) {
            second = safety;
        }
    });
    out.blendedSafety =
        (best * static_cast<long double>(config.weight1) +
         second * static_cast<long double>(config.weight2)) /
        static_cast<long double>(config.weight1 + config.weight2);
    return out;
}

inline SecondarySafetyEvaluator::Result::Candidate SecondarySafetyEvaluator::evaluate(
    ObservedBoard& board, Basic::Result& basic,
    Structure::Result& structure, const Probability::Result& prob,
    Structure::ShapePool& shapePool, Distribution::DistPool& pool, CellId cell,
    long double bestWeight, const Config& config) {
    const auto [x, y] = board.pos(cell);

    Result::Candidate out;
    out.x = x;
    out.y = y;
    const Probability::ObserveResult observation =
        Exact::observe(board, basic, structure, prob, pool, cell);
    out.safety = 1.0L - observation.explosion;
    Basic::Delta updates;
    updates.upd.resize(1);
    updates.upd[0].cell = cell;

    long double secondarySafety = 0;
    long double progressProb = 0;
    long double expectedClears = 0;
    long double safetyThisTileLeft = out.safety;
    bool pruned = false;
    for (int v = 0; v <= 8; ++v) {
        const long double probV = observation.digit[v];
        if (probV == 0.0L) continue;

        // 乐观上界剪枝（Java doFullEvaluateTile 同款）：剩余概率全按 100%
        // 安全也追不上第一名。
        const long double progressBonus =
            1.0L + (progressProb + safetyThisTileLeft) * config.progressContribution;
        const long double optimistic =
            (secondarySafety + safetyThisTileLeft) * progressBonus;
        if (bestWeight > 0 && optimistic < bestWeight) {
            pruned = true;
            break;
        }

        // 临时局面：就地揭示 x=v，随后由 Delta 回滚。
        board.board[x][y] = static_cast<Cell>(v);
        updates.upd[0].next = static_cast<Cell>(v);
        const Basic::Delta basicDelta = Basic::update(board, basic, updates);
        const Structure::Delta structureDelta =
            Structure::update(board, basic, structure, shapePool, updates);
        Probability::Result pv = Exact::analyze(board, basic, structure, pool);

        const FrontierSummary summary = summarizeFrontier(pv, board, structure, config);

        expectedClears += summary.clears * probV;
        secondarySafety += probV * summary.blendedSafety;
        if (summary.clears > 0.0L) progressProb += probV;
        safetyThisTileLeft -= probV;

        Structure::applyDelta(structure, structureDelta, true);
        Basic::applyDelta(basic, basicDelta, true);
        board.board[x][y] = Cell::Hidden;
    }

    out.expectedClears = expectedClears;
    out.weight = secondarySafety *
                 (1.0L + progressProb * config.progressContribution);
    if (pruned) out.weight = 0.0L;
    return out;
}

}  // namespace mss
