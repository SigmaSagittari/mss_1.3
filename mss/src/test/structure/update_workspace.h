// test/structure/update_workspace.h — Structure::update 的工作区卫生检查。
//
// 协议背景（workspaces.h）：update 专用缓冲采用"函数内使用、退出前手动清
// 本轮碰过的格子"的生命周期协议——收尾不干净会让下一次 update 的
// markDirty / collectComponent 短路，或让"收尾全 0"归纳不变量失效。本模块
// 在每个局面快照（= 上一次 update 刚结束，Snapshot 生成前无其他 structure
// 操作）时遍历整个工作区，断言：
//   - structureWs.dirty / updateVis / updateCellHash 全 0（本轮碰过的
//     格子 = dirtyCells ∪ 新组件成员，update 收尾全部清回）；
//   - dirtyCells 为空。
// 明确不断言的：
//   - removedFlag：事务式缓冲（每轮开头 assign 清零）；收尾允许尾部删标
//     槽残留 1（step3.5 的 pop 路径不写）——协议已兜底，不为测试加收尾
//     清零的热路径写；
//   - hashBox / updateCells / buckets 等事务式缓冲：每轮入口 clear/assign，
//     无跨轮读，收尾残留无害。
// 全盘遍历是 O(nm)（每局面 3 遍网格），测试专用，不算算法成本；
// 别"优化"成只查脏区——那正是它要抓的 bug 本身。
#pragma once

#include "test/common.h"

namespace mss::test {

inline void testUpdateWorkspaceClean(const unsigned long long& seed,
                                     const TestConfig& config) {
    Rng rng(seed);
    long long positions = 0;
    const RunSummary summary = runGames(config, rng, [&](const Snapshot& snapshot) {
        const ObservedBoard& board = snapshot.game.board;
        const Basic::Result& basic = snapshot.analysis.basic;
        const Structure::Result& structure = snapshot.analysis.structure;
        StructureWs& ws = scratch::structureWs;

        for (int x = 1; x <= board.rows; ++x)
            for (int y = 1; y <= board.cols; ++y) {
                MSS_TEST_CHECK(ws.dirty[x][y] == 0, "dirty not cleared after update",
                               board, &basic, &structure);
                MSS_TEST_CHECK(ws.updateVis[x][y] == 0, "updateVis not cleared after update",
                               board, &basic, &structure);
                MSS_TEST_CHECK(ws.updateCellHash[x][y] == U128{},
                               "updateCellHash not cleared after update",
                               board, &basic, &structure);
            }
        MSS_TEST_CHECK(ws.dirtyCells.empty(), "dirtyCells not emptied after update",
                       board, &basic, &structure);
        ++positions;
    });
    std::cout << "structure/update-workspace: " << positions << " positions from "
              << summary.games << " games in " << summary.elapsedSeconds << "s\n";
}

}  // namespace mss::test