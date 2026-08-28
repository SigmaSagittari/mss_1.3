// mini_repro.cpp — 复现 it=13 初始盘面的 Structure::Analyzer 输出
#include <cstdio>
#include <vector>
#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/types.h"

using namespace mss;

int main() {
    // 13000_pre_update 的盘面（5x3, 5 mines）
    const int rows = 5, cols = 3;
    ObservedBoard b(rows, cols, 5);
    const char* grid[5] = {"1.0", ".31", "...", "3..", "1.2"};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 3; ++j)
            b.board[i + 1][j + 1] = grid[i][j] == '.' ? Cell::Hidden
                                                       : static_cast<Cell>(grid[i][j] - '0');
    const Basic::Result basic = Basic::Analyzer::analyze(b);
    std::printf("basic valid=%d u=%d m=%d\n", (int)basic.valid, basic.unknownSum,
                basic.mineSum);
    for (int i = 1; i <= rows; ++i) {
        for (int j = 1; j <= cols; ++j)
            std::printf("%d ", (int)basic.marks[i][j]);
        std::printf("\n");
    }
    Structure::ShapePool pool;
    const Structure::Result st = Structure::Analyzer::analyze(b, basic, pool);
    std::printf("components=%zu\n", st.components.size());
    for (std::size_t c = 0; c < st.components.size(); ++c) {
        const auto& inst = st.components[c];
        std::printf("  comp%zu hash=%016llx%016llx\n", c,
                    (unsigned long long)inst.shape->hash.hi,
                    (unsigned long long)inst.shape->hash.lo);
        for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
            std::printf("    box[%zu]:", bb);
            for (std::size_t k = inst.boxes.boxOf[bb]; k < inst.boxes.boxOf[bb + 1]; ++k) {
                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                std::printf(" (%d,%d)", x, y);
            }
            std::printf("\n");
        }
        std::printf("    cons:");
        for (CellId cid : inst.constraintCells) {
            const auto [x, y] = b.pos(cid);
            std::printf(" (%d,%d)", x, y);
        }
        std::printf("\n");
    }
    // cellLoc 覆盖检查
    int assigned = 0;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) {
            const CellLocation loc = st.cellLoc[static_cast<std::size_t>(b.id(i, j))];
            if (loc.component >= 0) ++assigned;
        }
    std::printf("assigned cells=%d\n", assigned);
    return 0;
}