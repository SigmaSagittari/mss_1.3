#pragma once

#include <cstdint>
#include <utility>

#include "core/utility/grid.h"

namespace mss {

// 盘面、分析层共享的基础类型。

// 盘面格子：0-8 为已翻开的数字，Hidden 为未翻开。
enum class Cell : int {
    Num0 = 0,
    Num1,
    Num2,
    Num3,
    Num4,
    Num5,
    Num6,
    Num7,
    Num8,
    Hidden = 9,
};

inline bool isNumber(Cell c) {
    int v = static_cast<int>(c);
    return v >= 0 && v <= 8;
}

inline int numberValue(Cell c) {
    return static_cast<int>(c);
}

// 稠密整数句柄。
using CellId = int;
using ComponentId = int;
using BoxId = int;
using ShapeId = int;
using DistributionId = int;

// 格子 → (所属连通块, shape 内单位格下标)。
// 不在任何连通块的格子（Safe/Mine/Unknown）component = -1。
struct CellLocation {
    ComponentId component = -1;
    BoxId box = -1;
};

// 遍历 (x, y) 的 8 个邻居，fn 收到的都是合法坐标。
template <typename Func>
inline void forEachAdjacent(int x, int y, int rows, int cols, Func&& fn) {
    bool up = x > 1;
    bool down = x < rows;
    bool left = y > 1;
    bool right = y < cols;

    if (up && left) fn(x - 1, y - 1);
    if (up) fn(x - 1, y);
    if (up && right) fn(x - 1, y + 1);
    if (left) fn(x, y - 1);
    if (right) fn(x, y + 1);
    if (down && left) fn(x + 1, y - 1);
    if (down) fn(x + 1, y);
    if (down && right) fn(x + 1, y + 1);
}

// 分析器输入：已揭示数字、未揭示格与总雷数。
struct ObservedBoard {
    int rows = 0;
    int cols = 0;
    int totalMines = 0;
    Grid<Cell> board;

    ObservedBoard() = default;

    ObservedBoard(int r, int c, int m)
        : rows(r), cols(c), totalMines(m), board(r, c, Cell::Hidden) {}

    // 与 Grid 布局一致，可用作按格存储的下标。
    CellId id(int x, int y) const {
        return x * (cols + 1) + y;
    }

    std::pair<int, int> pos(CellId cell) const {
        return {cell / (cols + 1), cell % (cols + 1)};
    }
};

}  // namespace mss
