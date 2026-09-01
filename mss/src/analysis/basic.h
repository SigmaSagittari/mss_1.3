#pragma once

#include <cstdint>
#include <vector>

#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// basic.h — 确定性推理：从数字盘面推出标记分类（Mine/Safe/Frontier/Unknown）。
//
// 类型全部嵌套在 Basic 下，分两类：
//   数据类（纯数据）：Mark / Result / Delta
//
// analyze            全量重建标记：Frontier 升级后，以数字格为起点泛洪，
//                    反复执行雷/安全饱和直至无新结论，再做合法性校验。
// update             增量更新：只处理"揭示"事件——被翻格标 Safe（已揭示，
//                    非推理，防止 structure 误当 Frontier）、邻格
//                    Unknown→Frontier；再由受影响数字格开始泛洪推导 Mine / Safe。
//                    返回携带 old 值的 Delta，供 applyDelta 撤销/重放。
// applyDelta         按 Delta 合入 result；reverse=true 逆序撤销。
//
// 层间：update 是公共资产，不绑定在数据实例上；Delta 同时携带原始事件与
// 推导出的标记变化，供搜索树撤销重放
// （applyDelta reverse）与 UI 增量消费；structure 从 Delta::upd 取脏区，
// 并直接读 result 当前状态（吃 DAG）。
// ─────────────────────────────────────────────────────────────

struct Basic {
    // ── 数据类 ──

    // 标记分类。
    enum class Mark : std::uint8_t {
        Safe = 0,     // 已揭示 / 确定安全
        Mine = 1,     // 确定是雷
        Frontier = 2, // 前沿：未开但有信息（邻接数字格）
        Unknown = 3,  // 无信息未开
    };

    // 全量分析结果。rows/cols 便于无 board 时自包含遍历与 CellId 反解。
    struct Result {
        int rows = 0;
        int cols = 0;
        Grid<Mark> marks;
        int unknownSum = 0;  // Unknown 格数量
        int mineSum = 0;     // Mine 格数量
        bool valid = true;   // 是否出现矛盾（无解）
    };

    // 增量：原始揭示事件、标记变化集合与应用后的统计。
    struct Delta {
        // 值事件：格子被翻开（next = 数字 0..8）或覆盖（next = Hidden）。
        struct updateCell {
            CellId cell = -1;
            Cell next = Cell::Hidden;
        };
        std::vector<updateCell> upd;

        // 单格标记变化。old 供 rollback 逆序恢复。
        struct Change {
            CellId cell = -1;
            Mark old = Mark::Unknown;
            Mark now = Mark::Unknown;
        };
        std::vector<Change> changes;
        int unknownSum = 0;
        int mineSum = 0;
        bool valid = true;
        // 应用前统计（update 开头记录；applyDelta(reverse=true) 撤销时恢复用）。
        int oldUnknownSum = 0;
        int oldMineSum = 0;
        bool oldValid = true;
    };

    // 全量重建（首次初始化或需要重新对齐时）。
    static Result analyze(const ObservedBoard& board);

    // 增量更新：就地修改 result（只碰受影响格子，无整盘拷贝），返回
    // Delta（含 old 值，供 applyDelta 撤销/重放）。前置：board 已被外部
    // （game/搜索层）更新为揭示后的状态，updates 列出哪些格子变了、变为什么。
    static Delta update(const ObservedBoard& board, Result& result, const Delta& updates);

    // 把 Delta 落到另一份 result（重放/同步；标记与统计以 Delta 为准）。
    // reverse=true 为撤销：状态须恰处于"应用该 delta 后"（LIFO）——逆序
    // 遍历 changes 置回 old，统计恢复应用前值。搜索树游走退出用。
    static void applyDelta(Result& result, const Delta& delta, bool reverse = false);
};

// ── 实现区 ──

inline Basic::Result Basic::analyze(const ObservedBoard& state) {
    using Mark = Basic::Mark;
    Result result;
    result.rows = state.rows;
    result.cols = state.cols;
    result.marks.resize(state.rows, state.cols, Mark::Safe);
    const auto& board = state.board;
    const int n = state.rows;
    const int m = state.cols;

    // 1. 初始化：未开格 → Unknown。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (board[i][j] == Cell::Hidden)
                result.marks[i][j] = Mark::Unknown;

    // 2. 数字周围的未开格升级为 Frontier（有信息）。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j]))
                forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                    if (board[nx][ny] == Cell::Hidden)
                        result.marks[nx][ny] = Mark::Frontier;
                });

    // 3. 从每个数字格开始泛洪。任一新 Mine / Safe 都会重新检查相邻数字，
    //    直到雷饱和与安全饱和都不再产生新结论。
    std::vector<CellId> pending;
    pending.reserve(static_cast<std::size_t>(n * m));
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j])) pending.push_back(state.id(i, j));

    for (std::size_t head = 0; head < pending.size(); ++head) {
        const auto [x, y] = state.pos(pending[head]);
        int mineCount = 0, candidateCount = 0;
        forEachAdjacent(x, y, n, m, [&](int nx, int ny) {
            if (result.marks[nx][ny] == Mark::Mine) ++mineCount;
            else if (board[nx][ny] == Cell::Hidden && result.marks[nx][ny] != Mark::Safe)
                ++candidateCount;
        });
        const int remaining = numberValue(board[x][y]) - mineCount;
        if (remaining < 0 || remaining > candidateCount) {
            result.valid = false;
            continue;
        }
        if (remaining != 0 && remaining != candidateCount) continue;

        const Mark mark = remaining == 0 ? Mark::Safe : Mark::Mine;
        forEachAdjacent(x, y, n, m, [&](int nx, int ny) {
            if (board[nx][ny] != Cell::Hidden || result.marks[nx][ny] == mark ||
                result.marks[nx][ny] == Mark::Mine || result.marks[nx][ny] == Mark::Safe)
                return;
            result.marks[nx][ny] = mark;
            forEachAdjacent(nx, ny, n, m, [&](int ax, int ay) {
                if (isNumber(board[ax][ay])) pending.push_back(state.id(ax, ay));
            });
        });
    }

    // 4. 合法性检验：对每个数字，去掉周围安全格后，
    //    数字必须落在 [周围确定雷数, 未定格数] 区间内。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j])) {
                int mineCount = 0, candidateCount = 0;
                forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                    if (result.marks[nx][ny] == Mark::Mine) mineCount++;
                    if (board[nx][ny] == Cell::Hidden && result.marks[nx][ny] != Mark::Safe)
                        if (result.marks[nx][ny] != Mark::Mine) candidateCount++;
                });
                const int v = numberValue(board[i][j]);
                if (v < mineCount || v > mineCount + candidateCount) {
                    result.valid = false;
                    break;
                }
            }

    // 5. 统计。
    result.unknownSum = result.mineSum = 0;
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j) {
            if (result.marks[i][j] == Mark::Mine) result.mineSum++;
            if (result.marks[i][j] == Mark::Unknown) result.unknownSum++;
        }

    return result;
}

inline Basic::Delta Basic::update(const ObservedBoard& board, Result& result,
                                  const Delta& updates) {
    using Mark = Basic::Mark;
    Delta delta;
    delta.upd = updates.upd;
    delta.oldUnknownSum = result.unknownSum;
    delta.oldMineSum = result.mineSum;
    delta.oldValid = result.valid;
    const int rows = board.rows;
    const int cols = board.cols;
    std::vector<CellId> pending;

    // 标记某格：就地改 result + 维护统计 + 记录变化（含 old）。每次变化都令
    // 相邻数字重新入队，驱动后续安全/危险格的泛洪判断。
    auto setMark = [&](int x, int y, Mark m) {
        Mark& cur = result.marks[x][y];
        if (cur == m) return;
        const Mark oldMark = cur;
        if (cur == Mark::Unknown) --result.unknownSum;
        if (cur == Mark::Mine) --result.mineSum;
        if (m == Mark::Unknown) ++result.unknownSum;
        if (m == Mark::Mine) ++result.mineSum;
        cur = m;
        delta.changes.push_back({board.id(x, y), oldMark, m});
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (isNumber(board.board[nx][ny])) pending.push_back(board.id(nx, ny));
        });
    };

    // 只支持"揭示"类更新（opens 一个隐藏格为数字 0..8）。
    // 回滚（next == Cell::Hidden）已移除：它会解除数字饱和、把已标 Mine 的
    // 格翻回 Frontier，需要第二轮传播（两环）才闭环，破坏"组件原子性"——
    // 纯揭示世界里标记只会单调收敛，数字格队列可将其影响泛洪到整个相关组件。
    for (const Delta::updateCell& u : updates.upd)
        assert_(u.next != Cell::Hidden,
                "Basic::update: 不支持回滚更新（next 必须为数字 0..8）");

    for (const Delta::updateCell& u : updates.upd) {
        const auto [x, y] = board.pos(u.cell);

        // 翻开数字：被翻格标 Safe（已揭示，非推理），邻居隐藏格接入前沿。
        setMark(x, y, Mark::Safe);
        if (isNumber(board.board[x][y])) pending.push_back(u.cell);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            // 邻居隐藏格：Unknown → Frontier。
            if (board.board[nx][ny] == Cell::Hidden && result.marks[nx][ny] == Mark::Unknown)
                setMark(nx, ny, Mark::Frontier);
        });
    }

    // 只重算受事件或标记变化影响的数字格；每次新结论会把相邻数字继续入队。
    for (std::size_t head = 0; head < pending.size(); ++head) {
        const auto [x, y] = board.pos(pending[head]);
        int mineCount = 0, candidateCount = 0;
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (result.marks[nx][ny] == Mark::Mine) ++mineCount;
            else if (board.board[nx][ny] == Cell::Hidden && result.marks[nx][ny] != Mark::Safe)
                ++candidateCount;
        });
        const int remaining = numberValue(board.board[x][y]) - mineCount;
        if (remaining < 0 || remaining > candidateCount) {
            result.valid = false;
            continue;
        }
        if (remaining != 0 && remaining != candidateCount) continue;

        const Mark mark = remaining == 0 ? Mark::Safe : Mark::Mine;
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (board.board[nx][ny] != Cell::Hidden || result.marks[nx][ny] == mark ||
                result.marks[nx][ny] == Mark::Mine || result.marks[nx][ny] == Mark::Safe)
                return;
            setMark(nx, ny, mark);
        });
    }

    delta.unknownSum = result.unknownSum;
    delta.mineSum = result.mineSum;
    delta.valid = result.valid;
    return delta;
}

inline void Basic::applyDelta(Result& result, const Delta& delta, bool reverse) {
    if (reverse) {
        // 撤销：逆序回放标记（同格多次变化时逆序回退整链），统计恢复应用前值。
        for (std::size_t i = delta.changes.size(); i-- > 0;) {
            const Delta::Change& c = delta.changes[static_cast<std::size_t>(i)];
            const int x = c.cell / (result.cols + 1);
            const int y = c.cell % (result.cols + 1);
            result.marks[x][y] = c.old;
        }
        result.unknownSum = delta.oldUnknownSum;
        result.mineSum = delta.oldMineSum;
        result.valid = delta.oldValid;
        return;
    }
    for (const Delta::Change& c : delta.changes) {
        const int x = c.cell / (result.cols + 1);
        const int y = c.cell % (result.cols + 1);
        result.marks[x][y] = c.now;
    }
    result.unknownSum = delta.unknownSum;
    result.mineSum = delta.mineSum;
    result.valid = delta.valid;
}

}  // namespace mss
