#pragma once

#include <array>
#include <cstdint>
#include <utility>
#include <vector>

#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/grid.h"
#include "core/utility/hash.h"

// ─────────────────────────────────────────────────────────────
// workspaces.h — 分析层各模块的线程工作区（scratch space）集中存放处。
//
// 规则：
//   - 每个模块一套 struct，字段 = 该模块全部函数的常驻缓冲；实例由下方
//     `inline thread_local` 提供——C++17 inline 保证全程序每线程一份
//     （头文件多处包含不重定义）。
//   - 模块头自己 `using scratch::structureWs` 拿自己的那份（using 是编译
//     期名字解析，零运行时开销）。
//   - 共享边界：只有"事务式缓冲"（每轮进入 clear/assign 重置，无跨调用
//     残留）才能跨函数共享；有"生命周期协议"的缓冲必须各占一份——
//     如 analyze 的 vis/cellHash 靠下次调用 fill 清零，update 的靠每次
//     收尾手动清，两者绝不能合并（曾因此触发增量重建错乱）。
// ─────────────────────────────────────────────────────────────

namespace mss {

struct Distribution;  // 前向声明：ExactWs 只存 const Distribution*（指针不需完整类型）

// structure.h 的工作区。vis/cellHash/cells 按函数拆两套（协议不同）：
//   - analyze：调用开头整体 fill(0)，之后只标记；
//   - update：函数内使用、退出前手动清"本轮碰过的格子"。
// 其余（dirty/dirtyCells/removedFlag 与 buildComponent 全套）都是事务式
// 缓冲，每轮重置，共享安全。
struct StructureWs {
    // analyze 专用
    Grid<char> analyzeVis;
    Grid<U128> analyzeCellHash;
    std::vector<std::pair<int, int>> analyzeCells;   // collectComponent 工作队列
    // update 专用
    Grid<char> updateVis;
    Grid<U128> updateCellHash;
    std::vector<std::pair<int, int>> updateCells;    // collectComponent 工作队列
    Grid<char> dirty;                                // 脏区标记
    std::vector<std::pair<int, int>> dirtyCells;     // 脏格列表
    std::vector<char> removedFlag;                   // 组件删标（与组件数组平行）
    // buildComponent（事务式：每轮 clear/assign）
    FlatHashTable<U128, BoxId, U128Hash> hashBox;    // 单位格哈希 → BoxId
    std::vector<BoxId> boxOfCells;                   // 格子 → BoxId（与单元格列表平行）
    // 每 box 的格桶：单 box ≤ 9 格（组内格子共邻任一数字，全落其 8 邻域 → ≤8，
    // 留 1 余量）⇒ 定长 array 免嵌套小分配；桶数随 box 数自然增长、容量跨轮复用。
    std::vector<std::array<CellId, 9>> buckets;
    std::vector<std::uint8_t> bucketSize;            // 各桶实际格数（清空按它，不按 9 清）
    std::vector<BoxId> boxCursor;                    // cells 平铺直填游标：第 b 个 box 的写位
    std::vector<char> boxUsed;                       // 约束去重标记
    std::vector<BoxId> allBoxIds;                    // 邻盒 id 平铺收集桶
};

// distribution.h 的工作区。全部为事务式缓冲（每次进入 assign 重置），
// forEachAssignment / Solver::analyze 共享安全（互不嵌套）。
struct DistributionWs {
    // forEachAssignment
    std::vector<std::vector<int>> boxLimits;         // box → 所属约束列表
    std::vector<int> consSum;                        // 约束和
    std::vector<int> consMaxAdd;                     // 约束可分配上限
    std::vector<int> curSum;                         // 增量约束剪枝：当前和
    std::vector<int> sizeSum;                        // 增量约束剪枝：已赋 size 和
    std::vector<char> assignment;                    // 深分配结果（caller 只读）
    // Solver::analyze
    std::vector<long double> wayTable;               // 雷数 → 方案数聚合
    std::vector<long double> expectFlat;             // 雷数 × box → 期望聚合
};

// exact.h 的工作区。analyze 与 observe 互不嵌套，共享安全；全部字段
// "调用期内使用"（开头 clear/assign 重置），无跨调用生命周期协议。
// 多项式池：analyze 的全积 pH、轮转目标 mult、当前块 pi、商 ti、除法余数
// rem；observe 的其余部分积 pRest、全盘积 pAll（pi/mult 与 analyze 共用）。
struct ExactWs {
    // 多项式缓冲（生成函数多项式）：start 表示最低次幂，coeffs[i] 是
    // x^(start+i) 的系数。纯数据容器；运算实现在 exact.h（Exact 内以
    // using Poly = ExactWs::Poly 使用）。
    struct Poly {
        int start = 0;
        std::vector<long double> coeffs;
    };
    // 多项式池
    Poly pH, mult, pi, ti, rem;
    Poly pRest, pAll;
    // analyze 用
    std::vector<const Distribution*> distList;   // 组件分布句柄（下标 = ComponentId）
    std::vector<long double> boxProb;            // 每分布取到概率
    // observe 用
    std::vector<ComponentId> captured;           // 被抓住连通块（去重）
    std::vector<char> seen;                      // 组件 → 已捕获标记
    std::vector<int> u;                          // 各 box 与 x 相邻的格数
    std::vector<std::array<long double, 9>> acc; // 分配 → (y, h) 权重聚合
    std::vector<long double> dp, ndp;            // 转移表卷积双缓冲
    std::vector<long double> f;                  // 其余部分恰有 t 雷的摆法数
    // 转移表（平铺）：每张表占 [off, off+cnt)，ranges 按表序记录
    struct Transfer {
        int h;  // 邻域雷数贡献
        int y;  // 块雷数
        long double w;
    };
    std::vector<Transfer> tran;
    std::vector<std::pair<int, int>> tranRanges;
};

namespace scratch {
inline thread_local StructureWs structureWs;
inline thread_local DistributionWs distributionWs;
inline thread_local ExactWs exactWs;
}  // namespace scratch

}  // namespace mss