#include <functional>
#include "common.h"
#include "tests.h"

// ── 测试 9：Delta 可逆性（applyDelta(reverse=true) 往返）──
// 形态：随机盘面 + 随机揭示的树状路径（游走：进入子节点 = 应用增量，
// 退出 = 撤销），每次退出后与进入前快照逐字段精确比较；最后回到根与
// 初始状态比较。全对局路径模拟（搜索树游走的骨架操作）。

namespace {

// 结构状态精确相等：组件顺序即身份（下标序、shape 指针、数据、cellLoc 逐格）。
bool structsEqual(const Structure::Result& a, const Structure::Result& b) {
    if (a.components.size() != b.components.size()) return false;
    if (a.cellLoc.size() != b.cellLoc.size()) return false;
    for (std::size_t c = 0; c < a.components.size(); ++c) {
        const auto& A = a.components[c];
        const auto& B = b.components[c];
        if (A.shape != B.shape) return false;
        if (A.boxes.boxOf != B.boxes.boxOf) return false;
        if (A.boxes.cells != B.boxes.cells) return false;
        if (A.constraintCells != B.constraintCells) return false;
    }
    for (std::size_t k = 0; k < a.cellLoc.size(); ++k)
        if (a.cellLoc[k].component != b.cellLoc[k].component ||
            a.cellLoc[k].box != b.cellLoc[k].box)
            return false;
    return true;
}

bool basicsEqual(const Basic::Result& a, const Basic::Result& b) {
    if (a.rows != b.rows || a.cols != b.cols) return false;
    if (a.unknownSum != b.unknownSum || a.mineSum != b.mineSum || a.valid != b.valid)
        return false;
    for (int i = 1; i <= a.rows; ++i)
        for (int j = 1; j <= a.cols; ++j)
            if (a.marks[i][j] != b.marks[i][j]) return false;
    return true;
}

bool boardsEqual(const ObservedBoard& a, const ObservedBoard& b) {
    if (a.rows != b.rows || a.cols != b.cols || a.totalMines != b.totalMines) return false;
    for (int i = 1; i <= a.rows; ++i)
        for (int j = 1; j <= a.cols; ++j)
            if (a.board[i][j] != b.board[i][j]) return false;
    return true;
}

}  // namespace

void testDeltaReverse(Gen& g, int iter) {
    T::section("T9 delta reverse roundtrip");
    for (int it = 0; it < iter; ++it) {
        const int rows = 3 + g.rng.below(3), cols = 3 + g.rng.below(3);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        std::vector<char> trueMine(static_cast<std::size_t>(rows * cols), 0);
        for (int f : rb.trueMines) trueMine[static_cast<std::size_t>(f)] = 1;

        ObservedBoard board = toLibBoard(rb);
        Basic::Result basic = Basic::Analyzer::analyze(board);
        Structure::ShapePool pool;
        Structure::Result structure = Structure::Analyzer::analyze(board, basic, pool);

        // 全部可揭示格（非雷隐藏；跨分支复用）。
        std::vector<std::pair<int, int>> hiddenOf;
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j)
                if (board.board[i][j] == Cell::Hidden &&
                    !trueMine[static_cast<std::size_t>(rb.flat(i, j))])
                    hiddenOf.emplace_back(i, j);

        // 路径状态（搜索树游走的单份状态：board/basic/structure 三件套）。
        struct Trio {
            ObservedBoard b;
            Basic::Result k;
            Structure::Result s;
        };
        Trio cur{board, basic, structure};

        // 树状游走：随机 1..2 个子分支 × 深度递减。
        // 进入子节点 = board 改格 + Basic/Structure::update（就地，delta 留存）；
        // 退出 = Structure/Basic::applyDelta(reverse=true) + board 写回 Hidden；
        // 退出后必须与进入前快照逐字段一致。
        std::function<void(int)> walk = [&](int depth) {
            if (depth <= 0) return;
            std::vector<std::pair<int, int>> avail;
            for (const auto& [i, j] : hiddenOf)
                if (cur.b.board[i][j] == Cell::Hidden) avail.emplace_back(i, j);
            if (avail.empty()) return;
            const int branches = 1 + static_cast<int>(g.rng.below(2));
            for (int k = 0; k < branches; ++k) {
                const auto [i, j] = avail[static_cast<std::size_t>(g.rng.below(
                    static_cast<int>(avail.size())))];
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (trueMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++v;
                });
                const Trio snap = cur;  // 进入前快照
                cur.b.board[i][j] = toCell(v);
                const Basic::Update u{cur.b.id(i, j), cur.b.board[i][j]};
                const Basic::Delta bd = Basic::Updater::update(
                    cur.b, cur.k, std::vector<Basic::Update>{u});
                const Structure::Delta sd = Structure::Updater::update(
                    cur.b, cur.k, cur.s, pool, std::vector<Basic::Update>{u});
                walk(depth - 1);  // 递归子分支
                Structure::Updater::applyDelta(cur.s, sd, true);
                Basic::Updater::applyDelta(cur.k, bd, true);
                cur.b.board[i][j] = Cell::Hidden;
                CHECK((boardsEqual(cur.b, snap.b) && basicsEqual(cur.k, snap.k) &&
                       structsEqual(cur.s, snap.s)),
                      "T9: reverse mismatch it=%d depth=%d branch=%d", it, depth, k);
            }
        };
        walk(2 + static_cast<int>(g.rng.below(2)));  // 深 2..3

        // 回到根：与初始状态一致。
        CHECK((boardsEqual(cur.b, board) && basicsEqual(cur.k, basic) &&
               structsEqual(cur.s, structure)),
              "T9: root not restored it=%d", it);
    }
}