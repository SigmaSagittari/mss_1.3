#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "analysis/basic.h"
#include "analysis/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// midgame/secondary_safety_evaluator.h — 中盘猜点：选"点开哪个格"。
//
// 用法：没有确定安全格时，一行
//     SecondarySafetyEvaluator::Result r = solve(board, basic, structure, prob, shapePool, pool);
//     得到推荐格 r.x, r.y（每个候选的细节在 r.candidates）。
//
// 思路：每个候选格，假装翻开了它，看翻完后局面好不好走，按结果打分。
//   打分 = 每种翻出数字出现的概率 × 那个新局面的"最好/次好安全度"，
//   再加一点"翻完还能继续开"的奖励。翻完路好走的格优先点。
//
// 来源：Java 版 SecondarySafetyEvaluator（MineSweeperSolver），
//   删去 50/50 相关部分；参数、行为一律以 Java 版为准。
// ─────────────────────────────────────────────────────────────

struct SecondarySafetyEvaluator {
    // 参数与 Java 版完全一致（SolverSettings.java 默认值，无同名别的口径）。
    struct Config {
        // "翻完还能继续开"的格，加分多少。0.001 = 几乎不加分，只在你开的
        //  分数一模一样时帮你挑一个。做语料 A/B 时才动。
        long double progressContribution = 0.001L;

        // 候选窗主门槛：安全度比"最强的活格"低 0.10 以内（注：安全度 =
        //  1 − 雷概率，这里是两者相减，不是比例；例：最强安全 0.95，则
        //  安全度 ≥ 0.85 的格全收进来评比）。
        long double selectionThreshold1 = 0.10L;

        // 候选窗兜底线：上面门槛收进来还不到 2 个候选时，把线放宽到
        //  低 0.20 以内（例：0.95 → ≥ 0.75）多收几个，保证至少有 2 个候选
        //  可比。注意：是候选太少才放宽，不是候选太多。
        long double selectionThreshold2 = 0.20L;

        // 候选格所在的"未开连片区域"小于 8 格 → 排到后面，先评估大区域的格
        //  （点开大区域容易翻 0、连锁展开，收益高）。全部候选都在小区域时才
        //  轮到它们。8 = 格子个数。
        int spaceThreshold = 8;

        // "翻成数字 v 后的新局面"用什么数代表：最好的格 ×4 + 次好的格 ×1，
        //  除以 5。改它 = 改对"后续最优格"的信任配比，一般不用动。
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
            long double secondarySafety = 0;  // "翻完后路好不好走"的总分
            long double progressProb = 0;     // "翻完能继续开"的概率
            long double expectedClears = 0;   // 期望连带开出的格数（只作参考，不参与打分）
            long double weight = 0;     // 最终分 = secondarySafety × (1 + cont×progressProb)
            bool certainProgress = false; // 不管翻出几，都有格必然能开
            bool dead = false;          // 翻开值唯一固定（点它没新信息）
            bool pruned = false;        // 评估途中被剪枝放弃（分数是估值下限）
        };

        int x = 0;                      // 推荐点击格（1-based；无候选 = 0,0）
        int y = 0;
        long double weight = 0;         // 推荐格的分数
        std::vector<Candidate> candidates;  // 全部候选（按分数降序）
        long long engineCalls = 0;      // 实际花了几次引擎调用（analyze 次数）
    };

    // 求解一次：候选窗 → 逐格评估 → 剪枝 → 返回分数最高的格。
    // prob 必须是同一局面的 Probability::Result。
    // shapePool：评估中会临时揭示格子、产生新形状，必须 intern（同 Structure::Updater）。
    // config 必传（Config{} 即全部默认；带类内默认成员初始化器的类型不支持
    //   = Config{} 形式的默认参数，这是 C++ 规则）。
    static Result solve(const ObservedBoard& board, const Basic::Result& basic,
                        const Structure::Result& structure,
                        const Probability::Result& prob,
                        Structure::ShapePool& shapePool,
                        Distribution::DistPool& pool,
                        const Config& config);

private:
    // 单格评估（solve 的辅助；bestWeight = 当前最高分，供剪枝用；0 = 尚无第一名）。
    static Result::Candidate evaluate(const ObservedBoard& board,
                                      const Basic::Result& basic,
                                      const Structure::Result& structure,
                                      const Probability::Result& prob,
                                      Structure::ShapePool& shapePool,
                                      Distribution::DistPool& pool, CellId cell,
                                      long double bestWeight,
                                      const Config& config);

    // 局面里"确定安全盒"的总格数（Java getClearCount；Java 的
    //   livingClearCount 还要排除死格，这里用全量——只影响 progressProb 的
    //   阈值比较，语料 A/B 验证差异）。
    static long double clearCount(const Probability::Result& p,
                                  const Structure::Result& s);

    // 局面里"最优/次优安全度"的 4:1 混合（Java getBlendedSafety；
    //   离网安全度 1−tCellProbability 作起点）。
    static long double blendedSafety(const Probability::Result& p,
                                     const Config& config);
};

// ── 实现区 ──
//
// evaluate（对齐 Java doFullEvaluateTile）：
//   1. 预检：临时把 x 标成 Safe → Structure 全量重建（intern 新形状）→
//      analyze 1 次 → 安全盒列表。存在不含 x 且格数 > 1 的安全盒 →
//      支配快速路径：weight = P(安全) × (1 + cont × P(安全))，返回。
//   2. 主循环：对 v = 邻域已确定雷数 .. +邻域未开格数：
//      临时把 x 揭示为数字 v（board 单格赋值 + Basic/Structure Delta 增量
//      更新）→ analyze 1 次 → 得 sol_v（新局面的候选方案数）→
//      prob_v = sol_v / 当前方案数；clears_v（新局面活清除数）；
//      新局面最优/次优安全度 → 丢弃临时局面（局部变量，无撤销）。
//   3. 汇总：secondarySafety = Σ prob_v × blendedSafety_v；
//      progressProb = Σ prob_v（仅 clears_v > 预检必清数 的值）；
//      weight = secondarySafety × (1 + cont × progressProb)。
//   4. 只有 1 个正概率 v → dead = true；每个正概率 v 都存在安全盒 →
//      certainProgress（Java commonClears 非空的近似）。
//   5. 剪枝：bestWeight 已足够高，且"已累计分数 + 剩余概率全按 100%
//      安全"仍追不上 → pruned = true 提前返回（weight 记 0，排序垫底）。

inline long double SecondarySafetyEvaluator::clearCount(const Probability::Result& p,
                                                        const Structure::Result& s) {
    long double n = 0;
    for (std::size_t cid = 0; cid < p.components.size(); ++cid) {
        const auto& cr = p.components[cid];
        const Structure::Shape& shape = *s.components[cid].shape;
        for (std::size_t b = 0; b < cr.boxProbs.size(); ++b)
            if (cr.boxProbs[b] == 0.0L)
                n += static_cast<long double>(shape.boxes[b].size);
    }
    return n;
}

inline long double SecondarySafetyEvaluator::blendedSafety(const Probability::Result& p,
                                                           const Config& config) {
    // 起点 = 离网安全度（Java offEdgeSafety 同款）。
    long double best = 1.0L - p.tCellProbability;
    long double second = best;
    for (const Probability::ComponentResult& cr : p.components)
        for (long double boxProb : cr.boxProbs) {
            const long double s = 1.0L - boxProb;
            if (s > best) {
                second = best;
                best = s;
            } else if (s > second) {
                second = s;
            }
        }
    return (best * static_cast<long double>(config.weight1) +
            second * static_cast<long double>(config.weight2)) /
           static_cast<long double>(config.weight1 + config.weight2);
}

inline SecondarySafetyEvaluator::Result::Candidate SecondarySafetyEvaluator::evaluate(
    const ObservedBoard& board, const Basic::Result& basic,
    const Structure::Result& structure, const Probability::Result& prob,
    Structure::ShapePool& shapePool, Distribution::DistPool& pool, CellId cell,
    long double bestWeight, const Config& config) {
    using Mark = Basic::Mark;
    const auto [x, y] = board.pos(cell);
    assert_(board.board[x][y] == Cell::Hidden, "SecondarySafety: 已揭示格不可评估");
    assert_(basic.marks[x][y] != Mark::Mine, "SecondarySafety: 确定雷不可评估");

    Result::Candidate out;
    out.x = x;
    out.y = y;
    out.safety = 1.0L - prob.mineProbability(cell, board, basic, structure);

    // ── 预检：x 必空局面（x 标 Safe，不动 board、不引入 witness）──
    Basic::Result bSafe = basic;
    {
        Mark& m = bSafe.marks[x][y];
        if (m == Mark::Unknown) --bSafe.unknownSum;
        else if (m == Mark::Mine) --bSafe.mineSum;  // 强制非雷：释放雷预算
        m = Mark::Safe;
    }
    Structure::Result sSafe = Structure::Analyzer::analyze(board, bSafe, shapePool);
    Probability::Result pSafe = Exact::analyze(board, bSafe, sSafe, pool);

    // 预检必清数（Java linkedTilesCount；x 已不在任何池子，自然不含 x）。
    const long double linkedClears = clearCount(pSafe, sSafe);

    // ── 支配快速路径（Java L258-270）：存在不含 x 且格数 > 1 的安全盒 → 简化评分。
    // x 标 Safe 后不属任何组件，故任一大小 > 1 的 0 概率盒都满足"不含 x"。
    bool dominated = false;
    for (std::size_t cid = 0; cid < pSafe.components.size() && !dominated; ++cid) {
        const auto& cr = pSafe.components[cid];
        const Structure::Shape& shape = *sSafe.components[cid].shape;
        for (std::size_t b = 0; b < cr.boxProbs.size(); ++b)
            if (cr.boxProbs[b] == 0.0L && shape.boxes[b].size > 1) {
                dominated = true;
                break;
            }
    }
    if (dominated) {
        out.secondarySafety = 0.0L;
        out.progressProb = 0.0L;
        out.weight = out.safety *
                     (1.0L + out.safety * config.progressContribution);
        out.certainProgress = true;  // 必出安全大盒，推进有保障
        return out;
    }

    // ── 主循环：枚举 x 翻开后每个可能数字 v ──
    int minesGot = 0, hiddenAdj = 0;
    forEachAdjacent(x, y, board.rows, board.cols, [&](int nx, int ny) {
        if (basic.marks[nx][ny] == Mark::Mine) ++minesGot;
        if (board.board[nx][ny] == Cell::Hidden) ++hiddenAdj;
    });
    const int minMines = minesGot;
    const int maxMines = minesGot + hiddenAdj;

    long double safetyThisTileLeft = out.safety;
    int validValues = 0;
    bool pruned = false;
    bool allVHaveSafeBox = true;  // certainProgress 的近似：每个正概率 v 都有安全盒

    for (int v = minMines; v <= maxMines; ++v) {
        // 乐观上界剪枝（Java L353-363）：剩余概率全按 100% 安全也追不上第一名。
        const long double progressBonus =
            1.0L + (out.progressProb + safetyThisTileLeft) * config.progressContribution;
        const long double optimistic =
            (out.secondarySafety + safetyThisTileLeft) * progressBonus;
        if (bestWeight > 0 && optimistic < bestWeight) {
            pruned = true;
            break;
        }

        // 临时局面：board 揭示 x=v + Basic/Structure 增量更新。
        ObservedBoard bd = board;
        bd.board[x][y] = static_cast<Cell>(v);
        Basic::Result bv = basic;
        const std::vector<Basic::Update> updates{
            Basic::Update{cell, static_cast<Cell>(v)}};
        Basic::Updater::update(bd, bv, updates);
        Structure::Result sv = structure;
        Structure::Updater::update(bd, bv, sv, shapePool, updates);
        Probability::Result pv = Exact::analyze(bd, bv, sv, pool);

        const long double sol = pv.candidates;
        if (sol == 0.0L) continue;  // 这个值不可能出现
        ++validValues;
        const long double probV = sol / prob.candidates;
        const long double clearsV = clearCount(pv, sv);

        out.expectedClears += clearsV * probV;
        out.secondarySafety += probV * blendedSafety(pv, config);
        if (clearsV > linkedClears) out.progressProb += probV;
        if (clearsV == 0.0L) allVHaveSafeBox = false;
        safetyThisTileLeft -= probV;
    }

    out.certainProgress = allVHaveSafeBox;
    out.weight = out.secondarySafety *
                 (1.0L + out.progressProb * config.progressContribution);
    out.dead = (validValues == 1);
    out.pruned = pruned;
    if (pruned) out.weight = 0.0L;  // 剪枝格垫底，solve 排序时会跳过
    return out;
}

}  // namespace mss
