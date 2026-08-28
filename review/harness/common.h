// common.h — harness 共享基础设施：测试统计（T/CHECK）、近似比较、
// 随机一致盘面生成、参考盘面 → 库盘面桥接。
// 注意：跨 TU 共享的变量/函数一律 inline（C++17），保证全程序单实例。
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "analysis/bruteforce/endgame_bruteforce.h"
#include "analysis/distribution.h"
#include "analysis/probability/exact.h"
#include "analysis/probability/probability.h"
#include "analysis/structure.h"
#include "core/assert.h"
#include "core/config.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"
#include "core/utility/radix_sort.h"
#include "core/utility/rng.h"

#include "ref_mining.h"

using namespace mss;  // 与原单文件 harness 一致：测试代码直接使用 mss:: 类型

// ── 测试统计（inline：多 TU 共享一份，CHECK 全程序累计）──
namespace T {
inline int checks = 0;
inline int fails = 0;
inline std::string cur;
inline void section(const char* name) { cur = name; }
inline void fail(const std::string& msg) {
    ++fails;
    if (fails <= 30) std::printf("  [FAIL] %s: %s\n", cur.c_str(), msg.c_str());
}
}  // namespace T

#define CHECK(cond, ...)                                        \
    do {                                                        \
        ++T::checks;                                            \
        if (!(cond)) {                                          \
            char buf[512];                                      \
            std::snprintf(buf, sizeof(buf), __VA_ARGS__);       \
            T::fail(std::string(buf));                          \
        }                                                       \
    } while (0)

// 近似相等（默认相对容差 1e-9，可覆盖）。
inline bool approx(long double a, long double b, long double tol = 1e-9L) {
    const long double d = std::fabs(a - b);
    const long double s = std::fabs(a) + std::fabs(b);
    return d <= tol * (s > 1.0L ? s : 1.0L);
}

inline Cell toCell(int v) {
    if (v < 0) return Cell::Hidden;
    return static_cast<Cell>(v);
}

// 参考盘面 → 库盘面。
inline ObservedBoard toLibBoard(const ref::RefBoard& r) {
    ObservedBoard b(r.rows, r.cols, r.totalMines);
    for (int i = 1; i <= r.rows; ++i)
        for (int j = 1; j <= r.cols; ++j) b.board[i][j] = toCell(r.at(i, j));
    return b;
}

// ── 生成一致的随机盘面（真实雷位 → 数字）──
struct Gen {
    ref::Rng rng;
    explicit Gen(std::uint64_t seed) : rng(seed) {}
};

inline ref::RefBoard genConsistent(Gen& g, int rows, int cols, int mineCount,
                                   int extraHiddenMax) {
    ref::RefBoard b(rows, cols, mineCount);
    std::vector<std::pair<int, int>> all;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) all.emplace_back(i, j);
    std::shuffle(all.begin(), all.end(), std::mt19937(g.rng.u32()));
    std::vector<char> mine(static_cast<std::size_t>(rows * cols), 0);
    for (int k = 0; k < mineCount; ++k) {
        const int f = b.flat(all[k].first, all[k].second);
        mine[static_cast<std::size_t>(f)] = 1;
        b.trueMines.push_back(f);
    }
    // 数字格 + 额外隐藏（非雷格随机隐藏，产生 Unknown/T 格）
    std::vector<std::pair<int, int>> nonMines;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (!mine[static_cast<std::size_t>(b.flat(i, j))]) nonMines.emplace_back(i, j);
    std::shuffle(nonMines.begin(), nonMines.end(), std::mt19937(g.rng.u32()));
    const int extra = std::min(static_cast<int>(nonMines.size()),
                               g.rng.below(extraHiddenMax + 1));
    std::vector<char> extraHide(static_cast<std::size_t>(rows * cols), 0);
    for (int k = 0; k < extra; ++k)
        extraHide[static_cast<std::size_t>(b.flat(nonMines[k].first, nonMines[k].second))] = 1;
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j) {
            if (mine[static_cast<std::size_t>(b.flat(i, j))] ||
                extraHide[static_cast<std::size_t>(b.flat(i, j))]) {
                b.at(i, j) = -1;
                continue;
            }
            int v = 0;
            ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                if (mine[static_cast<std::size_t>(b.flat(ni, nj))]) ++v;
            });
            b.at(i, j) = v;
        }
    return b;
}