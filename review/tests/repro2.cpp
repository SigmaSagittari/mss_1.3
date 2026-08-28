// repro2.cpp — 回归：纯揭示序列下 Structure::Updater::update == 全量重析 + 无崩溃。
// 原崩溃场景（回滚序列）已被库断言禁止（next 必须为数字）；本复现改为揭示序列，
// 覆盖曾经的崩溃盘面（it=13 前身），每步断言语义等价（box/约束顺序无关）。
#include <algorithm>
#include <cstdio>
#include <map>
#include <tuple>
#include <vector>

#include "analysis/basic.h"
#include "analysis/structure.h"
#include "core/assert.h"
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

// 语义等价（box 编号顺序无关）：组件 token = 排序的 box 格集 + 排序的约束多集
static bool semEquals(const Structure::Result& a, const Structure::Result& b, int rows,
                      int cols) {
    using BoxSet = std::vector<CellId>;
    using Token = std::tuple<std::vector<BoxSet>,
                             std::vector<std::pair<int, std::vector<BoxSet>>>>;
    auto token = [&](const Structure::Instance& c) {
        std::vector<BoxSet> boxes;
        for (std::size_t b2 = 0; b2 < c.boxes.count(); ++b2) {
            BoxSet s(c.boxes.cells.begin() + c.boxes.boxOf[b2],
                     c.boxes.cells.begin() + c.boxes.boxOf[b2 + 1]);
            std::sort(s.begin(), s.end());
            boxes.push_back(std::move(s));
        }
        std::sort(boxes.begin(), boxes.end());
        std::vector<BoxSet> perBox(boxes.size());
        for (std::size_t b2 = 0; b2 < c.boxes.count(); ++b2) {
            BoxSet s(c.boxes.cells.begin() + c.boxes.boxOf[b2],
                     c.boxes.cells.begin() + c.boxes.boxOf[b2 + 1]);
            std::sort(s.begin(), s.end());
            perBox[b2] = std::move(s);
        }
        std::vector<std::pair<int, std::vector<BoxSet>>> cons;
        for (const auto& lim : c.shape->constraints) {
            std::vector<BoxSet> refs;
            for (BoxId bid : lim.boxIds) refs.push_back(perBox[static_cast<std::size_t>(bid)]);
            std::sort(refs.begin(), refs.end());
            cons.emplace_back(lim.sum, std::move(refs));
        }
        std::sort(cons.begin(), cons.end());
        return Token(std::move(boxes), std::move(cons));
    };
    std::map<Token, int> ta, tb;
    for (const auto& c : a.components) ta[token(c)]++;
    for (const auto& c : b.components) tb[token(c)]++;
    return ta == tb;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    // 曾经的崩溃盘面（it=13）：(5,2)=2 已揭示；(3,1)(3,2)(4,2)(4,3)(2,1) 为雷
    const char* grid[5] = {"1.0", ".31", "...", "3..", "122"};
    ObservedBoard b = mk(grid, 5, 3, 5);
    const bool mine[5][3] = {{false, false, false}, {true,  false, false},
                             {true,  true,  false}, {false, true,  true},
                             {false, false, false}};
    Basic::Result basic = Basic::Analyzer::analyze(b);
    Structure::ShapePool pool;
    Structure::Result root = Structure::Analyzer::analyze(b, basic, pool);

    const struct {
        int x, y;
    } reveals[] = {{2, 2}, {3, 3}, {1, 2}};
    int fails = 0;
    for (const auto& r : reveals) {
        if (b.board[r.x][r.y] != Cell::Hidden) continue;
        int v = 0;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                const int nx = r.x + dx, ny = r.y + dy;
                if (nx >= 1 && nx <= 5 && ny >= 1 && ny <= 3 && mine[nx - 1][ny - 1]) ++v;
            }
        b.board[r.x][r.y] = static_cast<Cell>(v);
        std::vector<Basic::Update> ups;
        ups.push_back({b.id(r.x, r.y), b.board[r.x][r.y]});
        std::printf("reveal (%d,%d)->%d\n", r.x, r.y, v);
        Basic::Result nb = Basic::Analyzer::analyze(b);
        Structure::Updater::update(b, nb, root, pool, ups);
        const Structure::Result full = Structure::Analyzer::analyze(b, nb, pool);
        if (!semEquals(root, full, 5, 3)) {
            std::printf("  MISMATCH comps %zu vs %zu\n", root.components.size(),
                        full.components.size());
            ++fails;
        } else {
            std::printf("  ok comps=%zu\n", root.components.size());
        }
        basic = nb;
    }
    std::printf(fails == 0 ? "ALL OK\n" : "FAILS=%d\n", fails);
    return fails == 0 ? 0 : 1;
}