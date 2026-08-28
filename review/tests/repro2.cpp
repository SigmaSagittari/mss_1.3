// repro2.cpp — 用 13001_pre_update 的完整状态（含重复归属 comp0+comp1）调用 update((2,3)->H)
#include <cstdio>
#include <vector>
#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/types.h"

using namespace mss;

static ObservedBoard mk(const char* g[5], int rows, int cols, int mines) {
    ObservedBoard b(rows, cols, mines);
    for (int i = 0; i < rows; ++i)
        for (int j = 0; j < cols; ++j)
            b.board[i + 1][j + 1] = g[i][j] == '.' ? Cell::Hidden
                                                    : static_cast<Cell>(g[i][j] - '0');
    return b;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 初始盘面 = anal_0013：(5,2)=2，其余与 harness it=13 一致
    const char* grid[5] = {"1.0", ".31", "...", "3..", "122"};
    ObservedBoard b = mk(grid, 5, 3, 5);
    Basic::Result basic = Basic::Analyzer::analyze(b);
    std::printf("initial board:\n");
    for (int i = 1; i <= 5; ++i) {
        for (int j = 1; j <= 3; ++j)
            std::printf("%s ", b.board[i][j] == Cell::Hidden
                                   ? "."
                                   : std::to_string((int)b.board[i][j]).c_str());
        std::printf("\n");
    }
    std::printf("marks:\n");
    for (int i = 1; i <= 5; ++i) {
        for (int j = 1; j <= 3; ++j) std::printf("%d ", (int)basic.marks[i][j]);
        std::printf("\n");
    }
    Structure::ShapePool pool;
    Structure::Result root = Structure::Analyzer::analyze(b, basic, pool);
    std::printf("analyze comps=%zu\n", root.components.size());
    for (std::size_t c = 0; c < root.components.size(); ++c) {
        const auto& inst = root.components[c];
        std::printf(" c%zu boxes=", c);
        for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
            std::printf("[");
            for (std::size_t k = inst.boxes.boxOf[bb]; k < inst.boxes.boxOf[bb + 1]; ++k) {
                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                std::printf("(%d,%d)", x, y);
            }
            std::printf("]");
        }
        std::printf(" cons=");
        for (CellId c2 : inst.constraintCells) {
            const auto [x, y] = b.pos(c2);
            std::printf("(%d,%d)", x, y);
        }
        std::printf("\n");
    }

    // 步骤0：(5,2) -> H（与 harness 一致）
    {
        b.board[5][2] = Cell::Hidden;
        Basic::Result nb = Basic::Analyzer::analyze(b);
        std::vector<Basic::Update> ups;
        ups.push_back({b.id(5, 2), Cell::Hidden});
        Structure::Delta d = Structure::Updater::update(b, nb, root, pool, ups);
        std::printf("step0 removed=%zu added=%zu comps=%zu\n", d.removed.size(),
                    d.added.size(), root.components.size());
        std::printf("  [step0 done]\n");
        for (std::size_t c = 0; c < root.components.size(); ++c) {
            const auto& inst = root.components[c];
            std::printf(" c%zu boxes=", c);
            for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
                std::printf("[");
                for (std::size_t k = inst.boxes.boxOf[bb]; k < inst.boxes.boxOf[bb + 1]; ++k) {
                    const auto [x, y] = b.pos(inst.boxes.cells[k]);
                    std::printf("(%d,%d)", x, y);
                }
                std::printf("]");
            }
            std::printf(" cons=");
            for (CellId c2 : inst.constraintCells) {
                const auto [x, y] = b.pos(c2);
                std::printf("(%d,%d)", x, y);
            }
            std::printf("\n");
        }
        int cnt21 = 0;
        for (const auto& inst : root.components)
            for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb)
                for (std::size_t k = inst.boxes.boxOf[bb]; k < inst.boxes.boxOf[bb + 1]; ++k)
                    if (inst.boxes.cells[k] == b.id(2, 1)) ++cnt21;
        std::printf("BOX(2,1) appears %d times in components\n", cnt21);

        // 步骤1：(2,3) -> H
        b.board[2][3] = Cell::Hidden;
        Basic::Result nb2 = Basic::Analyzer::analyze(b);
        std::vector<Basic::Update> ups2;
        ups2.push_back({b.id(2, 3), Cell::Hidden});
        std::printf("step1 calling update (2,3)->H...\n");
        Structure::Delta d2 = Structure::Updater::update(b, nb2, root, pool, ups2);
        std::printf("step1 done removed=%zu added=%zu comps=%zu\n", d2.removed.size(),
                    d2.added.size(), root.components.size());
    }
    std::printf("OK, no crash\n");
    return 0;
}