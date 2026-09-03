#pragma once

#include <cstdint>
#include <cstring>
#include <iostream>

#include "test/common.h"

namespace mss::test {

// ─────────────────────────────────────────────────────────────
// move_hash.h — 招法级回归指纹：逐招输出整条决策链路的哈希。
//
// 用途：语义等价性验证。对同一固定种子同一配置，两轮运行（如工作区重构
// 前后）的输出必须逐行一致，证明改动"仅语义修改"。哈希覆盖盘面、basic
// 标记与统计、structure 实例（shape 指纹 + 位置数据）、概率结果、选中招
// 与是否必须猜——任何一层的行为变化都会反映为哈希变化。
//
// 基线用法：任何改动之后，只重新跑这个模块并对比下方基线即可，无需其他
// 校验。基线配置：seed = 0xC0FFEE12345ULL，30x30 / 225 雷 / 20000 局 /
// firstMoveSafe = true（即 harness.cpp 的 normal_test）。
//   基线：move-hash-total: b153b2b60f75dd7c (20000 games, 3198953 moves)
// ─────────────────────────────────────────────────────────────

// FNV-1a 64 位：按原始字节折叠，long double 也按存储位参与（MSVC 下 80 位
// 扩展精度存 16 字节），保证位级可比。
inline void hashBytes(std::uint64_t& h, const void* data, std::size_t size) {
    const auto* p = static_cast<const unsigned char*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
}

template <typename T>
inline void hashPod(std::uint64_t& h, const T& v) {
    hashBytes(h, &v, sizeof(T));
}

inline std::uint64_t hashSnapshot(const Snapshot& s) {
    std::uint64_t h = 0xcbf29ce484222325ULL;  // FNV-1a 64 offset basis
    const ObservedBoard& board = s.game.board;
    const Basic::Result& basic = s.analysis.basic;
    const Structure::Result& structure = s.analysis.structure;
    const Probability::Result& probability = s.analysis.probability;

    for (int x = 1; x <= board.rows; ++x)
        for (int y = 1; y <= board.cols; ++y)
            hashPod(h, board.board[x][y]);
    for (int x = 1; x <= board.rows; ++x)
        for (int y = 1; y <= board.cols; ++y)
            hashPod(h, basic.marks[x][y]);
    hashPod(h, basic.mineSum);
    hashPod(h, basic.unknownSum);
    hashPod(h, basic.valid);

    hashPod(h, structure.components.size());
    for (const Structure::Instance& inst : structure.components) {
        // shape 用内容指纹（无坐标、跨运行稳定），不用指针地址。
        hashPod(h, inst.shape->hash);
        hashPod(h, inst.boxes.boxOf.size());
        for (CellId c : inst.boxes.cells) hashPod(h, c);
        for (std::uint16_t v : inst.boxes.boxOf) hashPod(h, v);
        hashPod(h, inst.constraintCells.size());
        for (CellId c : inst.constraintCells) hashPod(h, c);
    }

    hashPod(h, probability.tCellProbability);
    hashPod(h, probability.candidates);
    hashPod(h, probability.components.size());
    for (const Probability::ComponentResult& cr : probability.components)
        for (long double p : cr.boxProbs) hashPod(h, p);

    hashPod(h, s.next.x);
    hashPod(h, s.next.y);
    hashPod(h, s.next.mineProbability);
    hashPod(h, s.mustGuess);
    return h;
}

// 局数/时间盒二选一驱动（同 TestConfig 规则）。**全部招法聚合成一个总哈希**：
// 每个招法哈希（同 hashSnapshot）按出现顺序继续混入累计值，运行结束只输出
// 一行 move-hash-total（含局数/招数）。两轮运行（如工作区重构前后）对比这
// 一个数即可：一致 = 仅语义修改。
inline void testMoveHashes(const unsigned long long& seed, const TestConfig& config) {
    Rng rng(seed);
    long long moves = 0;
    std::uint64_t total = 0xcbf29ce484222325ULL;  // 聚合起点（FNV offset basis）
    const RunSummary summary = runGames(config, rng, [&](const Snapshot& s) {
        const std::uint64_t h = hashSnapshot(s);
        hashPod(total, h);  // 每个招法哈希（8 字节）按序混入总哈希
        ++moves;
    });
    std::cout << "move-hash-total: " << std::hex << total << std::dec << " ("
              << summary.games << " games, " << moves << " moves)\n";
}

}  // namespace mss::test