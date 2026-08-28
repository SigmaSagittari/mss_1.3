#pragma once

#include <cstdint>
#include <vector>

#include "core/assert.h"
#include "core/types.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// basic.h — 盘面的确定性推理。
//
// 类型全部嵌套在 Basic 命名空间下，分两类：
//   数据类（纯数据，无方法）：Mark / Result / Update / Change / Delta
//   算法类（纯空壳，无成员、零开销）：Analyzer / Updater
//
// 职责：
//   - Analyzer::analyze(board)        全量重建标记（含 Safe 推理，oracle）。
//   - Updater::update(board, result, updates)
//                                     增量分析，返回 Delta，不改 result。
//                                     只做"切割"：标 Mine（数字==周围隐藏数）
//                                     与 Frontier/Unknown；不标 Safe、不泛洪。
//                                     翻开格标 Safe = "已揭示"，防止 structure
//                                     误当 Frontier 污染 box。
//   - Updater::applyDelta(result, delta)  把 Delta 合入 result。
//
// 层间：update 是公共资产，不绑定在数据实例上；Delta 供 undo journal 与
// UI 增量消费；structure 不消费 Delta，直接读 result 当前状态（吃 DAG）。
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

    // 值事件：格子被翻开（next = 数字 0..8）或覆盖（next = Hidden）。
    struct Update {
        CellId cell = -1;
        Cell next = Cell::Hidden;
    };


    // 增量分析结果：变化集合 + 应用后的统计。
    struct Delta {
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
    };

    // ── 算法类 ──

    struct Analyzer {
        // 全量重建（首次初始化或需要重新对齐时）。
        static Result analyze(const ObservedBoard& board);
    };

    struct Updater {
        // 增量更新：就地修改 result（只碰受影响格子，无整盘拷贝），
        // 返回 Delta（含 old 值，供 undo journal 逆序恢复）。
        // 前置条件：board 已被外部（game/搜索层）更新为揭示后的状态，
        // updates 说明哪些格子变了、变为什么。
        static Delta update(const ObservedBoard& board, Result& result,
                            const std::vector<Update>& updates);

        // 按 Delta 应用标记（标记与统计以 Delta 为准）。
        // 用于把预先算好的 Delta 落到另一份 result（重放/同步）。
        static void applyDelta(Result& result, const Delta& delta);
    };
};

// ── 实现区 ──

inline Basic::Result Basic::Analyzer::analyze(const ObservedBoard& state) {
    using Mark = Basic::Mark;
    Result result;
    result.rows = state.rows;
    result.cols = state.cols;
    result.marks.resize(state.rows, state.cols, Mark::Safe);
    const auto& board = state.board;
    const int n = state.rows;
    const int m = state.cols;

    // 1. 初始化：所有未开格标 Unknown。
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

    // 3. 雷的确定：数字 == 周围未开数量 → 周围都是雷。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j])) {
                int hiddenCount = 0;
                forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                    if (board[nx][ny] == Cell::Hidden) hiddenCount++;
                });
                if (hiddenCount == numberValue(board[i][j]))
                    forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                        if (board[nx][ny] == Cell::Hidden)
                            result.marks[nx][ny] = Mark::Mine;
                    });
            }

    // 4. 安全的确定：数字 == 周围确定雷数 → 剩余未开格都是安全。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j])) {
                int mineCount = 0;
                forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                    if (result.marks[nx][ny] == Mark::Mine) mineCount++;
                });
                if (mineCount == numberValue(board[i][j]))
                    forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                        if (board[nx][ny] == Cell::Hidden && result.marks[nx][ny] != Mark::Mine)
                            result.marks[nx][ny] = Mark::Safe;
                    });
            }

    // 5. 合法性检验：对每个数字，去掉周围安全格后，
    //    数字必须落在 [周围确定雷数, 未开格数] 区间内。
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j)
            if (isNumber(board[i][j])) {
                int mineCount = 0, openCount = 0;
                forEachAdjacent(i, j, n, m, [&](int nx, int ny) {
                    if (result.marks[nx][ny] == Mark::Mine) mineCount++;
                    if (board[nx][ny] == Cell::Hidden && result.marks[nx][ny] != Mark::Safe)
                        openCount++;
                });
                const int v = numberValue(board[i][j]);
                if (v < mineCount || v > openCount) {
                    result.valid = false;
                    break;
                }
            }

    // 6. 统计。
    result.unknownSum = result.mineSum = 0;
    for (int i = 1; i <= n; ++i)
        for (int j = 1; j <= m; ++j) {
            if (result.marks[i][j] == Mark::Mine) result.mineSum++;
            if (result.marks[i][j] == Mark::Unknown) result.unknownSum++;
        }

    return result;
}

inline Basic::Delta Basic::Updater::update(const ObservedBoard& board,
                                           Result& result,
                                           const std::vector<Update>& updates) {
    using Mark = Basic::Mark;
    Delta delta;
    const int rows = board.rows;
    const int cols = board.cols;

    // 标记某格：就地改 result + 维护统计 + 记录变化（含 old）。同标记跳过。
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
    };

    // 只支持"揭示"类更新（opens 一个隐藏格为数字 0..8）。
    // 回滚（next == Cell::Hidden）已移除：它会解除数字饱和、在事件第 2 环把
    // Mine 翻回 Frontier，破坏结构增量更新的"组件原子性"闭包（需两环标脏）。
    // 纯揭示世界里新标雷必在事件所属组件内，单环（事件+八邻域）即闭环。
    for (const Update& u : updates)
        assert_(u.next != Cell::Hidden,
                "Basic::Updater: 不支持回滚更新（next 必须为数字 0..8）");

    for (const Update& u : updates) {
        const auto [x, y] = board.pos(u.cell);

        // 翻开数字：被翻格标 Safe（已揭示，非推理），只加 Frontier / Mine。
        setMark(x, y, Mark::Safe);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            // 邻居隐藏格：Unknown → Frontier。
            if (board.board[nx][ny] == Cell::Hidden && result.marks[nx][ny] == Mark::Unknown)
                setMark(nx, ny, Mark::Frontier);

            // 数字 == 周围隐藏数 → 周围隐藏格全部是雷。
            if (!isNumber(board.board[nx][ny])) return;
            int hiddenCount = 0;
            forEachAdjacent(nx, ny, rows, cols, [&](int ax, int ay) {
                if (board.board[ax][ay] == Cell::Hidden) ++hiddenCount;
            });
            if (hiddenCount != numberValue(board.board[nx][ny])) return;
            forEachAdjacent(nx, ny, rows, cols, [&](int ax, int ay) {
                if (board.board[ax][ay] != Cell::Hidden) return;
                if (result.marks[ax][ay] == Mark::Mine) return;
                setMark(ax, ay, Mark::Mine);
            });
        });
    }

    delta.unknownSum = result.unknownSum;
    delta.mineSum = result.mineSum;
    delta.valid = result.valid;
    return delta;
}

inline void Basic::Updater::applyDelta(Result& result, const Delta& delta) {
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