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
// midgame/midgame_search.h — 中盘猜点搜索（v6 基线）。
//
// 职责：当前局面无确定性落子（无安全格、无必标雷）时，选出第一个点击的
// 格子；确定性收尾由搜索以外的既有逻辑处理。
//
// 算法（迭代加深 + 完全展开 + 0.9 硬截断/硬淘汰 + 兜底轮）：
//   树形：招法节点 → 候选（observe 分析单元，结局内嵌）→ 子招法节点。
//   每轮：全部叶子完全展开一层 → 全树自底向上快照 → 0.9 全域斩杀 → 终止
//   检查。终止出口：根候选列表 = 1 / 全终盘 / 预算不足（进入兜底轮）。
//   终止必然性：非终盘叶子每轮必产生子节点（存活候选 ≥ 2 个正概率结局；
//   窗口全死格时钦定生存概率最大的死格为唯一候选）；幸存路径深度 ≤
//   未开格数 − 雷数，到点剩余隐藏格全部是雷 → 全终盘。
//
// 状态传递（结构"搜索树节点增量重放"，见 structure.h）：
//   节点不存完整棋盘；每个招法节点存三样增量（父状态 → 本状态）：
//     reveal           揭示事件（board 差异 = 单格赋值）
//     Basic::Delta     Basic::Updater::update 的产物
//     Structure::Delta Structure::Updater::update 的产物
//   游走 = 一次 DFS 维护单份路径状态：进子节点 = 应用 reveal 与两个 Delta
//   （applyDelta）；退出 = 撤销（applyDelta(reverse=true)，LIFO）——不再
//   每叶从根重放、不拷贝整盘。可逆性依据：board 写回 Hidden；
//   Basic::Delta::Change 自带 old；Structure::Delta::removedData 保存被删
//   组件原数据（与 removed 槽位同序），按逆序换回。
//   不可重放的产物必须缓存（否则重放重复付费）：
//     Position::prob     Exact::analyze 产物（observe 每轮都要）
//     Candidate::observe Exact::observe 产物（死格判定 + 结局分布）
//
// 代价口径：预算 = 真实引擎调用数（时间盒折算），Exact::analyze（局面分析）
//   与 Exact::observe（结局观察）各计 1 单位。节点诞生 = 1 分析 + 其窗口逐
//   候选 observe（候选列表形成 + 死格判定 + H0）；展开第一波 = 每正概率结局
//   子节点 1 分析；根局面由 get 自带 1 次精确分析，预算口径自洽。
//
// 常数（阈值放各自层，config.h 只放跨层共享值）：0.9 规则、H0 混合 4:1、
//   次佳兜底 0 均为命名常量，调参靠重编译重测对比。
//
// 确定性：同输入必同输出；并列时死亡概率低者优先，再按坐标序（1-based）。
// ─────────────────────────────────────────────────────────────

struct MidgameSearch {
    // ── 搜索树 ──
    // 一次求解的树形结构（内部实现，调用方不触碰节点；get 是唯一读写方）。
    // 两种节点互持指针；Position 不存完整棋盘，靠 reveal + Delta 沿路径
    // 应用/撤销（游走）维护状态（见文件头"状态传递"）。
    // 生命周期：调用方构造空树传给 get，析构自动 DFS 整树回收（防泄漏）；
    // 也可显式 freeAll（析构会再调，幂等）。
    struct SearchTree {
        // 对外：构造 / 析构 / 显式回收；其余（节点类型与树管理）私有。
        // C++ 嵌套类对外围类并无私有访问权（[class.access.nest]），故以
        // friend 显式授予——get 是树状态的唯一读写方。
        friend struct MidgameSearch;
        SearchTree() = default;

        // 整树回收（DFS，避免内存泄漏）；析构自动调用，显式调用幂等。
        void freeAll();

        ~SearchTree();

    private:
        // 两种节点互指，先给前向声明（完整定义在下方）。
        struct Position;
        struct Candidate;

        // 招法节点（局面）：值 = H0（叶子）/ max V̂（已展开）/ 1（终盘）。
        struct Position {
            Position* parent = nullptr;       // 父招法节点（根 = nullptr）
            Basic::Update reveal;             // 相对父的揭示事件（根：cell = -1，无事件）
            int depth = 0;                    // 深度（= 沿路径的揭示数，同步于轮数）
            int unopened = 0;                 // 本局面未开格数（终止性上界 U−M 用）
            int mines = 0;                    // 本局面剩余雷数

            // 增量（父状态 → 本状态；创建时由 Updater::update 就地算好）。
            // 沿路径逐个 applyDelta 即得本局面完整状态；纯算术，不扣预算。
            Basic::Delta basicDelta;
            Structure::Delta structureDelta;

            Probability::Result prob;         // 本局面概率（analyze 的缓存，重建不重复付费）
            long double value = 0;            // 快照值
            bool terminal = false;            // 终盘（全部隐藏格 P(雷) ∈ {0,1}）
            std::vector<Candidate*> candidates;  // 候选列表（0.9 窗口 − 死格，斩杀后摘除）
        };

        // 候选（一次点击的分析单元）。运气节点与结局不单独建类型，
        // 结局内嵌于此：children[v] = 数字 v 的子招法节点（零概率不建）。
        struct Candidate {
            int x = 0;                        // 1-based 行
            int y = 0;                        // 1-based 列
            Probability::ObserveResult observe;  // 结局分布 + 爆炸概率（创建时付费缓存）
            bool dead = false;                // 观测值唯一（死格；判定后退池或钦定为唯一候选）
            long double value = 0;            // V̂ = Σ_v P(v)·Ŵ(children[v]) + 爆炸 × 0（快照）
            bool survived = true;             // 存活（未被 0.9 斩杀）
            std::array<Position*, 9> children{};  // 正概率数字结局 → 子节点（nullptr = 零概率）
        };

        Position* root = nullptr;

        // 新建节点（裸指针互连，树自行拥有，外部不得持有/释放）。
        Position* newPosition();
        Candidate* newCandidate();

        // 整枝回收（DFS）：先深度释放全部后代，再删候选，最后删节点本身。
        // 斩杀 = 父候选列表摘除 + 本函数（不双删：已摘除的候选不在任何
        // 父列表中，不会被再次遍历到）。
        void freeSubtree(Position* p);
    };

    // ── 一次求解的产物 ──
    // 报告类型全部内嵌，对齐 EndgameBruteforce::Result::Winrate 先例。
    struct Result {
        // 终止出口。
        enum class Reason {
            SingleSurvivor,   // 根候选列表存活数 = 1（自然收敛）
            AllTerminal,      // 全体叶子终盘（全部隐藏格已确定，必胜局面）→ 全树结算
            BudgetExhausted,  // 预算不足展开下一轮 → 兜底轮完成
            Degenerate,       // 预算不足根窗口观察（预算 < 根窗口格数）→ 按网格安全率直接报告
        };

        // 根候选报告，每候选一项；被淘汰候选保留淘汰轮的最后一次快照值。
        struct Candidate {
            int x = 0;                        // 1-based 行
            int y = 0;                        // 1-based 列
            long double value = 0;            // 终止时快照值 V̂（被淘汰者 = 淘汰轮最后快照值）
            long double deathProbability = 0; // P(踩雷)，observe 的 explosion
            long double survival = 0;         // Σ_v P(v|T)，应 = 1 − deathProbability（闭合校验）
            bool survived = false;            // 终止时存活（未被 0.9 斩杀）
        };

        // 每轮开销与斩杀统计。rounds 恰好 depth 项；若触发兜底轮则多一项，
        // 最后一项就是兜底轮（兜底轮不做斩杀，killedCandidates/removedNodes 为 0）。
        struct RoundStat {
            int round = 0;                // 轮号（1-based）
            long long cost = 0;           // 本轮引擎调用数（展开 + 本轮诞生的子节点开销）
            int killedCandidates = 0;     // 本轮被斩杀候选数（全域重筛）
            long long removedNodes = 0;   // 本轮删除节点数（含被斩候选的全部后代）
        };

        // 兜底轮被抛弃的叶子：不展开、不删除、保持旧值参与最终报告。
        struct AbandonedLeaf {
            int depth = 0;                // 该叶子所在深度
            long double value = 0;        // 保留值（H0 或旧快照值）
            long double deathProbability = 0;  // 叶子候选列表内最小 P(雷)（排序依据）
        };

        int x = 0;                        // 推荐点击格（1-based）
        int y = 0;
        Reason reason = Reason::BudgetExhausted;
        int depth = 0;                    // 评估深度（完成的轮数；Degenerate 时为 0）
        long long engineCalls = 0;        // 实际消耗
        long long budget = 0;             // 名义预算（入参原样回填）
        long long totalNodes = 0;         // 终止时树中节点总数（招法节点 + 候选 + 结局）
        std::vector<Candidate> candidates;      // 根候选报告（含被淘汰者，survived = false）
        std::vector<RoundStat> rounds;           // 每轮开销与斩杀统计
        std::vector<AbandonedLeaf> abandoned;    // 兜底轮被抛弃叶子（空 = 未触发兜底）
    };

    // 在无确定性落子的当前局面选择第一个点击的格子。
    // tree：本步求解的搜索树（调用方构造空树；析构自动整树回收）。
    // board / basic / structure：调用方的当前局面分析（与 solveEndgame 同款约定），
    //   也是沿路径重放的根副本基底。
    // shapePool / pool：调用方持有的结构池与分布池，跨调用复用；搜索树的新
    //   形状 intern 进 shapePool（只增不删）。
    // maxEngineCalls：预算，必填（≤ 0 = 调用错误）。
    static Result get(SearchTree& tree, const ObservedBoard& board,
                      const Basic::Result& basic, const Structure::Result& structure,
                      Structure::ShapePool& shapePool, Distribution::DistPool& pool,
                      long long maxEngineCalls);

private:


};

// ── 实现区（设计已定稿，代码待填充）──
//
// 内部流程（对齐项目先例：全部静态/局部、thread_local 复用缓冲）：
//   - 游走 walk(root, st)：一次 DFS 维护单份路径状态（PathState）。进子
//     节点 = enterChild（单格赋值 + 两个 applyDelta），退出 = leaveChild
//     （两个 applyDelta(reverse=true) + 写回 Hidden）；叶子处执行展开。
//   - 展开 expand(leaf, st)：
//       候选列表形成（节点诞生时已付成本）：0.9 窗口（概率网格筛，免费）→
//       逐候选 observe（1 单位，兼作死格判定与结局分布缓存）；窗口全死格 →
//       钦定生存概率最大者为唯一候选；H0 于是时计算。
//       第一波：每存活候选 × 每正概率结局 v → createChild（newPosition +
//       reveal{c,v} + 父态副本单格赋值 + 两个 update 就地算 delta + analyze
//       1 单位 + 终盘判定 + 子窗口观察）。
//   - 轮：游走全树展开全部叶子 → 自底向上快照（纯算术：候选
//     V̂ = Σ P·Ŵ + 爆炸×0；节点 = max V̂ / H0 / 1）→ 0.9 全域斩杀（纯算术
//     比较 + freeSubtree 回收）→ 终止检查 → 兜底轮（死亡概率升序，精确成本
//     ≤ 剩余预算，逐叶游走展开）。
//   - 常数（放本层）：kCutRatio = 0.9L（窗口与斩杀共用）、H0 混合权重 4:1、
//     H0 次佳兜底 0。

}  // namespace mss