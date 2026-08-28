// ref_mining.h — 独立参考实现（供差分测试使用，不包含任何库逻辑）。
//
// 设计原则：所有参考算法从 Minesweeper 语义出发从零实现，
// 不 import 库的 Shape/Box/Constraint/结构分解等概念。
#pragma once

#include <cstdint>
#include <random>
#include <utility>
#include <vector>

namespace ref {

// 邻居（含对角，棋盘内）。
template <typename Fn>
inline void forEa(int i, int j, int rows, int cols, Fn&& fn) {
    for (int di = -1; di <= 1; ++di)
        for (int dj = -1; dj <= 1; ++dj) {
            if (di == 0 && dj == 0) continue;
            const int ni = i + di, nj = j + dj;
            if (ni >= 1 && ni <= rows && nj >= 1 && nj <= cols) fn(ni, nj);
        }
}

// 独立 RNG（xorshift64* + 不同常数），避免与被测库共用任何代码路径。
struct Rng {
    std::uint64_t s = 0x9E3779B97F4A7C15ULL;
    explicit Rng(std::uint64_t seed) : s(seed ? seed : 1) {}
    std::uint64_t next() {
        std::uint64_t x = s;
        x ^= x >> 12;
        x ^= x << 25;
        x ^= x >> 27;
        s = x;
        return x * 0x2545F4914F6CDD1DULL;
    }
    int below(int n) { return static_cast<int>(next() % static_cast<std::uint64_t>(n)); }
    // 返回 32 位随机整数，便于 std::shuffle。保证不重复调 next()。
    std::uint32_t u32() { return static_cast<std::uint32_t>(next()); }
};

// 参考盘面：1-based 坐标，边框保留。cell=-1 表示隐藏，0..8 表示数字。
struct RefBoard {
    int rows = 0;
    int cols = 0;
    int totalMines = 0;
    std::vector<int> cell;      // 下标 = (i)*(cols+2)+(j)，1-based，边框不可访问
    std::vector<int> trueMines; // 生成器真实雷位（flat 下标）——盘面自洽基准

    RefBoard() = default;
    RefBoard(int r, int c, int m) : rows(r), cols(c), totalMines(m) {
        cell.assign(static_cast<std::size_t>((r + 2) * (c + 2)), -1);
    }
    int& at(int i, int j) { return cell[static_cast<std::size_t>(i) * (cols + 2) + j]; }
    int at(int i, int j) const { return cell[static_cast<std::size_t>(i) * (cols + 2) + j]; }

    bool inB(int i, int j) const { return i >= 1 && i <= rows && j >= 1 && j <= cols; }
    int flat(int i, int j) const { return (i - 1) * cols + (j - 1); }
    // 某格的真实数字（若打开会显示什么）
    int trueDigit(int i, int j) const {
        int v = 0;
        forEa(i, j, rows, cols, [&](int ni, int nj) {
            for (int f : trueMines)
                if (f == flat(ni, nj)) { ++v; break; }
        });
        return v;
    }

    // 隐藏格坐标列表（flat 下标 → (i,j)）。
    std::vector<std::pair<int, int>> hiddenCells() const {
        std::vector<std::pair<int, int>> v;
        for (int i = 1; i <= rows; ++i)
            for (int j = 1; j <= cols; ++j)
                if (at(i, j) < 0) v.emplace_back(i, j);
        return v;
    }
};

// ── 参考枚举：全部满足约束且雷数==totalMines 的摆雷方案 ──
// 方案 = 隐藏格 flat 下标集合。上限 2^hidden 靠调用方控制盘面大小。
inline std::vector<std::vector<int>> enumeratePlacements(const RefBoard& b) {
    const auto hidden = b.hiddenCells();
    const int h = static_cast<int>(hidden.size());
    std::vector<std::vector<int>> out;
    std::vector<int> sel;
    sel.reserve(b.totalMines);

    // 校验：对每个数字格，周围雷数 == 数字值。
    auto valid = [&](const std::vector<int>& S) -> bool {
        std::vector<char> isMine(static_cast<std::size_t>(b.rows * b.cols), 0);
        for (int f : S) isMine[static_cast<std::size_t>(f)] = 1;
        for (int i = 1; i <= b.rows; ++i)
            for (int j = 1; j <= b.cols; ++j) {
                const int v = b.at(i, j);
                if (v < 0) continue;
                int cnt = 0;
                forEa(i, j, b.rows, b.cols, [&](int ni, int nj) {
                    if (b.at(ni, nj) >= 0) return;  // 已揭示，非雷
                    if (isMine[static_cast<std::size_t>(b.flat(ni, nj))]) ++cnt;
                });
                if (cnt != v) return false;
            }
        return true;
    };

    // 组合枚举 C(h, totalMines)。
    std::vector<int> comb;
    auto dfs = [&](auto&& self, int start, int need) -> void {
        if (need == 0) {
            if (valid(comb)) out.push_back(comb);
            return;
        }
        if (h - start < need) return;
        for (int f = start; f < h; ++f) {
            comb.push_back(b.flat(hidden[static_cast<std::size_t>(f)].first,
                                  hidden[static_cast<std::size_t>(f)].second));
            self(self, f + 1, need - 1);
            comb.pop_back();
        }
    };
    dfs(dfs, 0, b.totalMines);
    return out;
}

// ── 参考：全盘信息（每隐藏格雷概率、每个数格邻雷数） ──
struct RefInfo {
    // per-cell: 0 .. placements.size()
    std::vector<int> mineCount;    // flat 下标
    std::vector<int> safeCount;    // flat 下标（非雷）
    std::size_t total = 0;
};

inline RefInfo aggregate(const RefBoard& b, const std::vector<std::vector<int>>& ps) {
    RefInfo info;
    info.mineCount.assign(static_cast<std::size_t>(b.rows * b.cols), 0);
    info.safeCount.assign(static_cast<std::size_t>(b.rows * b.cols), 0);
    info.total = ps.size();
    for (const auto& S : ps)
        for (int f : S) info.mineCount[static_cast<std::size_t>(f)]++;
    for (const auto& S : ps) {
        std::vector<char> mk(static_cast<std::size_t>(b.rows * b.cols), 0);
        for (int f : S) mk[static_cast<std::size_t>(f)] = 1;
        for (int i = 1; i <= b.rows; ++i)
            for (int j = 1; j <= b.cols; ++j)
                if (b.at(i, j) < 0 && !mk[static_cast<std::size_t>(b.flat(i, j))])
                    info.safeCount[static_cast<std::size_t>(b.flat(i, j))]++;
    }
    return info;
}

// 参考：点开 (i,j) 的数字分布（x 非雷时）。返回 [k]=计数。
inline std::vector<std::size_t> observeDigits(const RefBoard& b,
                                              const std::vector<std::vector<int>>& ps,
                                              int i, int j) {
    std::vector<std::size_t> digits(9, 0);
    const int xf = b.flat(i, j);
    for (const auto& S : ps) {
        bool isMine = false;
        for (int f : S)
            if (f == xf) { isMine = true; break; }
        if (isMine) continue;
        std::vector<char> mk(static_cast<std::size_t>(b.rows * b.cols), 0);
        for (int f : S) mk[static_cast<std::size_t>(f)] = 1;
        int cnt = 0;
        forEa(i, j, b.rows, b.cols, [&](int ni, int nj) {
            if (b.at(ni, nj) < 0 && mk[static_cast<std::size_t>(b.flat(ni, nj))]) ++cnt;
        });
        digits[static_cast<std::size_t>(cnt)]++;
    }
    return digits;
}

}  // namespace ref