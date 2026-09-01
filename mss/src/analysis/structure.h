#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "analysis/basic.h"
#include "core/assert.h"
#include "core/types.h"
#include "core/utility/flat_hashtable.h"
#include "core/utility/hash.h"

namespace mss {

// ─────────────────────────────────────────────────────────────
// structure.h — 盘面图论结构：把 Frontier/数字格组织成连通块，供分布层计数。
//
// 核心拆分：
//   - Shape：interned、不可变、无棋盘坐标，hash 即身份键；同构连通块跨盘面
//     共享一份，分布按 shape 去重（Distribution 层挂第二个池
//     const Shape* → Distribution，不进 Instance）。
//   - Instance：每份 Result 各一个：Shape 观察指针 + 本盘面位置数据。
//
// 类型（全部嵌套在 Structure 下）：
//   数据类  Shape / Instance / Result / Delta
//   池      ShapePool（按 hash 去重，只增不删，Shape 地址稳定）
//   算法     analyze（全量）/ update（增量，就地改 Result 只碰脏块，
//           返回 Delta 供搜索树增量重放/撤销，不做墓碑式保留）
//
// 层间：分析接口吃 DAG——直接读 board + Basic::Result 的当前状态，
// 而非只吃上层 delta。
// ─────────────────────────────────────────────────────────────

struct Structure {
    // ── 数据类 ──

    // interned 不可变连通块形状。无棋盘坐标，hash 是内容指纹（身份键）。
    struct Shape {
        // 单位格规格：数字邻域集合相同的隐藏格组。只含 size，无位置。
        struct Box {
            int size = 0;
        };

        // 约束：一个数字格，要求其邻接单位格的雷数总和等于 sum。
        // boxIds 是 shape 内局部下标（0..boxes.size()-1）。
        struct Constraint {
            int sum = 0;
            std::vector<BoxId> boxIds;
        };

        std::vector<Box> boxes;
        std::vector<Constraint> constraints;
        U128 hash = {};  // 内容指纹（ShapePool 去重依据）
    };

    // 每盘面的连通块实例：引用 interned shape + 本盘面位置数据。
    struct Instance {
        // 扁平存储的性能优化：所有 box 的格子压进一个数组，用 boxOf 前缀和
        // 定位各 box 区间。避免 vector<vector<CellId>> 的嵌套分配与间接寻址。
        // 语义等价 vector<vector<CellId>>；对外只暴露 box 区间访问。
        struct Boxes {
            std::vector<CellId> cells;  // 全部格子，按 box 顺序扁平排列
            // boxOf 前缀和：第 b 个 box 的格子区间是 [boxOf[b], boxOf[b+1])。
            // 名字取 "offsets of boxes" 之义。
            std::vector<std::uint16_t> boxOf;

            std::size_t count() const { return boxOf.empty() ? 0 : boxOf.size() - 1; }
            std::size_t cellCount(std::size_t b) const {
                return boxOf[b + 1] - boxOf[b];
            }
        };

        const Shape* shape = nullptr;  // interned 形状（观察指针，池只增不删不悬垂）
        Boxes boxes;                   // 本盘面单位格的格子
        // 约束数字格，顺序与 shape.constraints 一致。
        std::vector<CellId> constraintCells;
    };

    // structure 段输出：全部连通块实例 + 格子 → 位置映射。
    struct Result {
        std::vector<Instance> components;   // 下标即 ComponentId
        std::vector<CellLocation> cellLoc;  // 按 CellId 索引
    };

    // 一次增量更新的变更集（搜索树增量重放 / UI 增量消费）。
    // removed 是"删除动作轨迹"：依执行顺序记录被删组件所处的数组槽位
    // （update 3.5 段 swap-pop，天然降序）；applyDelta 依序机械重放
    // （swap-pop + cellLoc 重映射）即与 update 的物理删除完全一致，无需排序。
    // removedData 与 removed 同序（pop 顺序）：各槽位的原组件数据——被覆盖/
    // 弹出后原数据即丢失，撤销时用它换回原组件。
    // added 与 addedData 对齐：added[i] = 真实状态里新增组件的 id，
    // addedData[i] = 该组件的完整数据（重放态没有别的来源，必须自带）。
    struct Delta {
        std::vector<ComponentId> removed;
        std::vector<Instance> removedData;
        std::vector<ComponentId> added;
        std::vector<Instance> addedData;
    };

    // ── 池 ──

    // 结构池：按 U128 hash 去重，只增不删。unique_ptr 保证 Shape 地址稳定，
    // 观察指针永不悬垂（池生命周期 = AnalysisContext 生命周期）。
    struct ShapePool {
        // 已存在同 hash 结构时返回既有指针；否则插入并返回新指针。
        const Shape* intern(Shape shape);

    private:
        // unique_ptr 保证 Shape 堆地址稳定；index_ 按 hash 反查 ShapeId。
        std::vector<std::unique_ptr<Shape>> shapes_;
        FlatHashTable<U128, ShapeId, U128Hash> index_;
    };

    // 全量构建：从 board + basic 标记推导全部连通块，intern 形状，写回 cellLoc。
    static Result analyze(const ObservedBoard& board, const Basic::Result& basic,
                          ShapePool& pool);

    // 增量更新：就地改 result（只碰受影响连通块，无整盘拷贝）。前置：board 与
    // basic 已被外部更新为揭示后的状态，updates 列出变化的格子（定位脏区）。
    static Delta update(const ObservedBoard& board, const Basic::Result& basic,
                        Result& result, ShapePool& pool, const Basic::Delta& updates);

    // 把 Delta 应用到另一份 Result（搜索树节点增量重放，从根沿路径逐个应用
    // 即得节点状态）。等价于 update 的结构变更部分，不触碰 board/basic（由调用方
    // 保持同步）。reverse=true 为撤销（LIFO）：先弹出被追加的 added 组件，再用
    // removedData 逆序换回被删组件。搜索树游走退出用。
    static void applyDelta(Result& result, const Delta& delta, bool reverse = false);

    // ── 实现区 ──

private:
    // 收集一个连通块的所有格子（数字格 + 前沿格）。
    static void collectComponent(int x, int y, const ObservedBoard& state,
                                 const Basic::Result& basic, Grid<char>& vis,
                                 std::vector<std::pair<int, int>>& cells);

    // 根据一个连通块的格子列表，构造它的 shape + 实例（intern 进 pool）。
    static Instance buildComponent(const std::vector<std::pair<int, int>>& cells,
                                   const ObservedBoard& state,
                                   const Basic::Result& basic, Grid<U128>& cellHash,
                                   ShapePool& pool);

    // 连通块结构哈希（128 位）：以 boxes / constraints 内容为指纹。
    static U128 computeHash(const Shape& shape);
};

// ── 实现区 ──

inline const Structure::Shape* Structure::ShapePool::intern(Shape shape) {
    shape.hash = Structure::computeHash(shape);
    if (const ShapeId* found = index_.find(shape.hash))
        return shapes_[*found].get();
    const ShapeId id = static_cast<ShapeId>(shapes_.size());
    shapes_.push_back(std::make_unique<Shape>(std::move(shape)));
    index_.emplace(shapes_[id]->hash, id);
    return shapes_[id].get();
}

inline Structure::Result Structure::analyze(const ObservedBoard& state,
                                            const Basic::Result& basic,
                                            ShapePool& pool) {
    using Mark = Basic::Mark;
    const int rows = state.rows;
    const int cols = state.cols;
    Result result;
    result.cellLoc.assign((rows + 1) * (cols + 1),
                          CellLocation{});

    // 线程局部复用缓冲区，避免反复分配。
    thread_local Grid<char> vis;
    thread_local Grid<U128> cellHash;
    thread_local std::vector<std::pair<int, int>> cells;
    if (vis.rows() != rows || vis.cols() != cols) {
        vis.resize(rows, cols, 0);
        cellHash.resize(rows, cols, U128{});
        cells.reserve(rows * cols / 2);
    } else {
        vis.fill(0);
        cellHash.fill(U128{});
    }

    // 1. 给数字格周围的未开格累加哈希值：哈希相同 = 属于同一单位格。
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (isNumber(state.board[i][j])) {
                const std::uint64_t pos =
                    static_cast<std::uint64_t>(i) * (cols + rows + 3) + j;
                const U128 seed{splitmix64(pos),
                                splitmix64(pos + 0x9e3779b97f4a7c15ULL)};
                forEachAdjacent(i, j, rows, cols, [&](int nx, int ny) {
                    cellHash[nx][ny] += seed;
                });
            }

    // 2. 从未访问的前沿格出发，收集所有连通块。
    for (int i = 1; i <= rows; ++i)
        for (int j = 1; j <= cols; ++j)
            if (basic.marks[i][j] == Mark::Frontier && !vis[i][j]) {
                cells.clear();
                collectComponent(i, j, state, basic, vis, cells);
                result.components.push_back(buildComponent(cells, state, basic, cellHash, pool));
            }

    // 3. 回填格子 → 位置映射。
    for (ComponentId cid = 0; cid < static_cast<ComponentId>(result.components.size());
         ++cid) {
        const Instance& inst = result.components[cid];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[inst.boxes.cells[k]] =
                    CellLocation{cid, static_cast<BoxId>(b)};
        for (CellId c : inst.constraintCells)
            result.cellLoc[c] = CellLocation{cid, -1};
    }

    return result;
}

inline void Structure::collectComponent(int x, int y, const ObservedBoard& state,
                                        const Basic::Result& basic, Grid<char>& vis,
                                        std::vector<std::pair<int, int>>& cells) {
    using Mark = Basic::Mark;
    // 双向扩散：数字格 → Frontier 邻格，Frontier 格 → 数字邻格。
    auto dfs = [&](auto&& self, int cx, int cy) -> void {
        if (vis[cx][cy]) return;
        vis[cx][cy] = 1;
        cells.emplace_back(cx, cy);

        if (isNumber(state.board[cx][cy])) {
            forEachAdjacent(cx, cy, state.rows, state.cols, [&](int nx, int ny) {
                if (basic.marks[nx][ny] == Mark::Frontier) self(self, nx, ny);
            });
        }
        if (basic.marks[cx][cy] == Mark::Frontier) {
            forEachAdjacent(cx, cy, state.rows, state.cols, [&](int nx, int ny) {
                if (isNumber(state.board[nx][ny])) self(self, nx, ny);
            });
        }
    };
    dfs(dfs, x, y);
}

inline Structure::Instance Structure::buildComponent(
    const std::vector<std::pair<int, int>>& cells, const ObservedBoard& state,
    const Basic::Result& basic, Grid<U128>& cellHash, ShapePool& pool) {
    using Mark = Basic::Mark;
    Instance inst;
    const int rows = state.rows;
    const int cols = state.cols;

    // 线程局部复用工作区（无重入）：避免每块重建的临时向量分配。
    static thread_local FlatHashTable<U128, BoxId, U128Hash> tlHashBox;
    static thread_local std::vector<int> tlHashUsed;
    static thread_local std::vector<BoxId> tlBoxOfCells;
    static thread_local std::vector<std::vector<CellId>> tlBuckets;
    static thread_local std::vector<char> tlBoxUsed;
    tlHashBox.clear();  // 保留容量，只清占位

    // 1. 收集单位格：哈希相同 = 同一单位格（哈希表替代 sort+unique+lower_bound，O(C) 均摊）。
    tlBoxOfCells.assign(cells.size(), static_cast<BoxId>(-1));
    Shape shape;

    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const auto [x, y] = cells[ci];
        if (basic.marks[x][y] != Mark::Frontier) continue;
        const U128 h = cellHash[x][y];
        BoxId boxId;
        if (const BoxId* found = tlHashBox.find(h)) {
            boxId = *found;
        } else {
            boxId = static_cast<BoxId>(shape.boxes.size());
            shape.boxes.push_back({0});
            tlHashBox.emplace(h, boxId);
        }
        shape.boxes[boxId].size++;
        tlBoxOfCells[ci] = boxId;
        // 复用 cellHash：改为保存 格子 → 单位格 id。
        cellHash[x][y] = U128{static_cast<std::uint64_t>(boxId), 0};
    }

    // 2. 按 box 顺序扁平收集格子（桶收集，O(C)，替代 box×cells 双循环）。
    tlBuckets.assign(shape.boxes.size(), {});
    for (std::size_t ci = 0; ci < cells.size(); ++ci) {
        const BoxId b = tlBoxOfCells[ci];
        if (b == static_cast<BoxId>(-1)) continue;  // 数字格无单位格归属
        tlBuckets[b].push_back(
            state.id(cells[ci].first, cells[ci].second));
    }
    inst.boxes.boxOf.push_back(0);
    for (std::size_t b = 0; b < tlBuckets.size(); ++b) {
        inst.boxes.cells.insert(inst.boxes.cells.end(), tlBuckets[b].begin(),
                                tlBuckets[b].end());
        inst.boxes.boxOf.push_back(static_cast<std::uint16_t>(inst.boxes.cells.size()));
    }

    // 3. 数字格 → 约束。
    tlBoxUsed.assign(shape.boxes.size(), 0);
    for (auto [x, y] : cells) {
        if (!isNumber(state.board[x][y])) continue;
        Shape::Constraint c;
        c.sum = numberValue(state.board[x][y]);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (basic.marks[nx][ny] == Mark::Mine) c.sum--;
            if (basic.marks[nx][ny] == Mark::Frontier) {
                const BoxId boxId = static_cast<BoxId>(cellHash[nx][ny].lo);
                if (!tlBoxUsed[boxId]) {
                    tlBoxUsed[boxId] = 1;
                    c.boxIds.push_back(boxId);
                }
            }
        });
        for (BoxId id : c.boxIds) tlBoxUsed[id] = 0;
        shape.constraints.push_back(std::move(c));
        inst.constraintCells.push_back(state.id(x, y));
    }

    // 4. intern 形状。
    inst.shape = pool.intern(std::move(shape));
    return inst;
}

inline U128 Structure::computeHash(const Shape& shape) {
    U128Hasher h;
    for (const auto& box : shape.boxes)
        h.mix(static_cast<std::uint64_t>(box.size));
    for (const auto& limit : shape.constraints) {
        h.mix(static_cast<std::uint64_t>(limit.sum));
        for (BoxId id : limit.boxIds)
            h.mix(static_cast<std::uint64_t>(id) + 0x9e3779b9ULL);
    }
    return h.finalize();
}

inline Structure::Delta Structure::update(const ObservedBoard& state,
                                          const Basic::Result& basic,
                                          Result& result, ShapePool& pool,
                                          const Basic::Delta& updates) {
    using Mark = Basic::Mark;
    Delta delta;
    const int rows = state.rows;
    const int cols = state.cols;

    // 线程局部复用缓冲区：尺寸变化时 resize；否则由函数末尾手动清零。
    static thread_local Grid<char> dirty;
    static thread_local Grid<char> vis;
    static thread_local Grid<U128> cellHash;
    static thread_local std::vector<std::pair<int, int>> dirtyCells;
    static thread_local std::vector<std::pair<int, int>> cells;
    static thread_local std::vector<char> removedFlag;  // 与组件数组平行的临时删标
    if (dirty.rows() != rows || dirty.cols() != cols) {
        dirty.resize(rows, cols, 0);
        vis.resize(rows, cols, 0);
        cellHash.resize(rows, cols, U128{});
        cells.reserve(rows * cols / 2);
    }
    // removedFlag 与组件数组平行，每次更新开头必须**整体**清零：
    // 只在数组增长时补零会残留上一轮的删标——把从未作废的活组件当垃圾删掉
    // （且 step2 的删标短路会跳过其合法作废），成员 cellLoc 残留脏归属。
    // assign(size, 0) 即全量清零。
    removedFlag.assign(result.components.size(), 0);

    // 标记某格为脏：去重，并记录位置到 dirtyCells。
    auto markDirty = [&](int x, int y) {
        if (dirty[x][y]) return;
        dirty[x][y] = 1;
        dirtyCells.emplace_back(x, y);
    };
    // 摘除某格的结构归属。
    auto clearCellLoc = [&](int x, int y) {
        result.cellLoc[state.id(x, y)] = CellLocation{};
    };
    // 作废一个组件：打删标、成员全部标脏并清归属（连通块是不可拆分的更新
    // 单位）。删标放在 update 内部的临时 removedFlag，不进公共 Instance 类型，
    // 避免随 Delta::addedData 泄漏给重放/UI 消费方。
    auto invalidateComponent = [&](ComponentId cid) {
        removedFlag[cid] = 1;
        const Instance& inst = result.components[cid];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [cx, cy] = state.pos(inst.boxes.cells[k]);
                markDirty(cx, cy);
                clearCellLoc(cx, cy);
            }
        for (CellId c : inst.constraintCells) {
            const auto [cx, cy] = state.pos(c);
            markDirty(cx, cy);
            clearCellLoc(cx, cy);
        }
    };

    // 1. 脏信号源：事件格 + 八邻域全部标脏（唯一入口）。
    //    只支持揭示更新（断言）：纯揭示世界里，新标雷必在"事件格所属组件"
    //    内（组件原子性闭环）、新 Frontier 必在八邻域内，单环即闭环。
    //    回滚更新（next==Hidden）已移除——它会在第二轮传播里解除 Mine 标记、
    //    把本是 Mine 的格翻回 Frontier，需要两环标脏才能覆盖。
    for (const Basic::Delta::updateCell& u : updates.upd)
        assert_(u.next != Cell::Hidden,
                "Structure::update: 不支持回滚更新（next 必须为数字 0..8）");
    for (const Basic::Delta::updateCell& u : updates.upd) {
        const auto [x, y] = state.pos(u.cell);
        markDirty(x, y);
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) { markDirty(nx, ny); });
    }

    // 2. 通过 cellLoc 反查脏格所属连通块，整块作废（打删标、清归属、成员标脏）。
    //    作废会向 dirtyCells 追加成员，故用动态下标遍历（删标去重，成员归位已清）。
    {
        std::size_t i = 0;
        while (i < dirtyCells.size()) {
            const auto [x, y] = dirtyCells[i++];
            const CellLocation loc = result.cellLoc[state.id(x, y)];
            if (loc.component == -1) continue;
            if (removedFlag[loc.component]) continue;  // 已作废
            invalidateComponent(loc.component);
        }
    }

    // 3. 重建脏区：只遍历 dirtyCells，从每个未访问的前沿脏格出发重建连通块。
    //    组件先暂存（staged）：必须先收齐全部删标（step2）并完成删除重排
    //    （step3.5），再按最终下标追加 + 回填 cellLoc。
    //    cellHash 只给本连通块涉及的格子算（与 build() 方向相反、结果一致）。
    auto hashAt = [&](int x, int y) {
        U128 h;
        forEachAdjacent(x, y, rows, cols, [&](int nx, int ny) {
            if (isNumber(state.board[nx][ny])) {
                const std::uint64_t pos =
                    static_cast<std::uint64_t>(nx) * (cols + rows + 3) + ny;
                h += U128{splitmix64(pos), splitmix64(pos + 0x9e3779b97f4a7c15ULL)};
            }
        });
        return h;
    };
    std::vector<Instance> staged;
    {
        std::size_t i = 0;
        while (i < dirtyCells.size()) {
            const auto [x, y] = dirtyCells[i++];
            if (basic.marks[x][y] != Mark::Frontier || vis[x][y]) continue;
            cells.clear();
            collectComponent(x, y, state, basic, vis, cells);
            // 无需"吞并"旧组件：step2 动态闭包已把事件波及的所有组件整块作废并
            // 清空其成员 cellLoc；纯揭示世界（assert next != Hidden）下，此处
            // DFS 访问的格子必然 loc.component == -1，重建归属不会重复。
            for (const auto& [cx, cy] : cells) cellHash[cx][cy] = hashAt(cx, cy);
            staged.push_back(buildComponent(cells, state, basic, cellHash, pool));
        }
    }

    // 3.5 真删除（无墓碑）：降序扫描删标，swap-pop + 重映射 cellLoc，同时把
    //     被删槽位依序写入 delta.removed（"动作轨迹"，天然降序）。
    //     降序是关键：处理到槽 i 时，所有已标删且 id > i 的组件均已弹出，当前
    //     尾部必为活组件——无条件重映射"被移动组件"的 cellLoc 才合法（升序会把
    //     仍在待删队列里的组件当尾部搬走并恢复其成员归属，产生脏 cellLoc）。
    //     交换时删标随元素迁移（降序下迁入的恒为活组件，迁移仅语义一致）。
    {
        int i = static_cast<int>(result.components.size()) - 1;
        while (i >= 0) {
            if (!removedFlag[i]) {
                --i;
                continue;
            }
            delta.removed.push_back(i);
            // 覆盖/弹出前拷贝原组件（applyDelta(reverse=true) 恢复用，与 removed 同序）。
            delta.removedData.push_back(result.components[i]);
            const ComponentId last =
                static_cast<ComponentId>(result.components.size()) - 1;
            if (i != last) {
                result.components[i] =
                    std::move(result.components[last]);
                removedFlag[i] =
                    removedFlag[last];
                // 重映射被移动组件的 cellLoc（尾部恒为活组件，见上）。
                const Instance& moved = result.components[i];
                for (std::size_t b = 0; b < moved.boxes.count(); ++b)
                    for (std::size_t k = moved.boxes.boxOf[b]; k < moved.boxes.boxOf[b + 1]; ++k)
                        result.cellLoc[moved.boxes.cells[k]] =
                            CellLocation{i, static_cast<BoxId>(b)};
                for (CellId c : moved.constraintCells)
                    result.cellLoc[c] = CellLocation{i, -1};
            }
            result.components.pop_back();
            --i;
        }
    }

    // 4. 追加暂存组件（最终下标 = 删除后的数组尾部），回填 cellLoc。
    const int appendStart = static_cast<int>(result.components.size());
    for (auto& inst : staged) {
        const int newIdx = static_cast<int>(result.components.size());
        result.components.push_back(std::move(inst));
        delta.added.push_back(newIdx);
        delta.addedData.push_back(result.components.back());  // 重放侧必须自带完整数据
        const Instance& written = result.components[newIdx];
        for (std::size_t b = 0; b < written.boxes.count(); ++b)
            for (std::size_t k = written.boxes.boxOf[b]; k < written.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[written.boxes.cells[k]] =
                    CellLocation{newIdx, static_cast<BoxId>(b)};
        for (CellId c : written.constraintCells)
            result.cellLoc[c] = CellLocation{newIdx, -1};
    }

    // 5. 清零工作区。清理集 = dirtyCells（事件邻域 + 被作废成员）∪ 本轮新建
    //    组件实例的成员坐标——后者即本次 DFS 的全部访问集（frontier →
    //    boxes.cells，数字格 → constraintCells），无需另存任何集合。
    for (const auto& [x, y] : dirtyCells) {
        dirty[x][y] = 0;
        vis[x][y] = 0;
        cellHash[x][y] = U128{};
    }
    dirtyCells.clear();
    for (int c = appendStart; c < static_cast<int>(result.components.size()); ++c) {
        const Instance& inst = result.components[c];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k) {
                const auto [x, y] = state.pos(inst.boxes.cells[k]);
                vis[x][y] = 0;
                cellHash[x][y] = U128{};
            }
        for (CellId cid : inst.constraintCells) {
            const auto [x, y] = state.pos(cid);
            vis[x][y] = 0;
            cellHash[x][y] = U128{};
        }
    }

    return delta;
}

// 把 Delta 应用到另一份 Result（搜索树节点增量重放）：与 update 的结构变更
// 部分完全等价——依序机械重放 delta.removed（swap-pop + cellLoc 重映射），
// 再按序追加 addedData；无需排序。前置：result 必须是产生该 Delta 时的那份
// 状态（从根沿路径应用即满足）。reverse=true 为撤销（须恰处于"应用该 delta
// 后"，LIFO——搜索树游走退出）：顺序与 apply 相反——先按逆序弹出被追加的
// added 组件（清其 cellLoc），再逆序恢复 removed 槽位：把槽位当前组件
// （apply 时从尾部搬来的）push_back 回尾部并重映射 cellLoc，用 removedData
// （与 removed 槽位列同序）换回原组件并回填其 cellLoc。
inline void Structure::applyDelta(Result& result, const Delta& delta, bool reverse) {
    if (reverse) {
        for (std::size_t n = delta.addedData.size(); n-- > 0;) {
            const Instance& victim = result.components.back();
            for (std::size_t b = 0; b < victim.boxes.count(); ++b)
                for (std::size_t k = victim.boxes.boxOf[b]; k < victim.boxes.boxOf[b + 1]; ++k)
                    result.cellLoc[victim.boxes.cells[k]] = CellLocation{};
            for (CellId c : victim.constraintCells)
                result.cellLoc[c] = CellLocation{};
            result.components.pop_back();
        }
        for (std::size_t k = delta.removed.size(); k-- > 0;) {
            const ComponentId i = delta.removed[k];
            const ComponentId tail = static_cast<ComponentId>(result.components.size());
            result.components.push_back(
                std::move(result.components[i]));
            const Instance& moved = result.components[tail];
            for (std::size_t b = 0; b < moved.boxes.count(); ++b)
                for (std::size_t j = moved.boxes.boxOf[b]; j < moved.boxes.boxOf[b + 1]; ++j)
                    result.cellLoc[moved.boxes.cells[j]] =
                        CellLocation{tail, static_cast<BoxId>(b)};
            for (CellId c : moved.constraintCells)
                result.cellLoc[c] = CellLocation{tail, -1};
            result.components[i] = delta.removedData[k];
            const Instance& restored = result.components[i];
            for (std::size_t b = 0; b < restored.boxes.count(); ++b)
                for (std::size_t j = restored.boxes.boxOf[b]; j < restored.boxes.boxOf[b + 1]; ++j)
                    result.cellLoc[restored.boxes.cells[j]] =
                        CellLocation{i, static_cast<BoxId>(b)};
            for (CellId c : restored.constraintCells)
                result.cellLoc[c] = CellLocation{i, -1};
        }
        return;
    }

    for (const ComponentId i : delta.removed) {
        // 先清被删组件的 cellLoc（与 update 的作废语义对齐）：否则重放后
        // 这些格子仍指向已删除/已挪走的组件，observe 会读到脏归属。
        const Instance& victim = result.components[i];
        for (std::size_t b = 0; b < victim.boxes.count(); ++b)
            for (std::size_t k = victim.boxes.boxOf[b]; k < victim.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[victim.boxes.cells[k]] = CellLocation{};
        for (CellId c : victim.constraintCells)
            result.cellLoc[c] = CellLocation{};
        const ComponentId last = static_cast<ComponentId>(result.components.size()) - 1;
        if (i != last) {
            result.components[i] =
                std::move(result.components[last]);
            // 重映射被移动组件的 cellLoc。
            const Instance& moved = result.components[i];
            for (std::size_t b = 0; b < moved.boxes.count(); ++b)
                for (std::size_t k = moved.boxes.boxOf[b]; k < moved.boxes.boxOf[b + 1]; ++k)
                    result.cellLoc[moved.boxes.cells[k]] =
                        CellLocation{i, static_cast<BoxId>(b)};
            for (CellId c : moved.constraintCells)
                result.cellLoc[c] = CellLocation{i, -1};
        }
        result.components.pop_back();
    }
    for (std::size_t i = 0; i < delta.addedData.size(); ++i) {
        const ComponentId cid = static_cast<ComponentId>(result.components.size());
        result.components.push_back(delta.addedData[i]);
        const Instance& inst = result.components[cid];
        for (std::size_t b = 0; b < inst.boxes.count(); ++b)
            for (std::size_t k = inst.boxes.boxOf[b]; k < inst.boxes.boxOf[b + 1]; ++k)
                result.cellLoc[inst.boxes.cells[k]] =
                    CellLocation{cid, static_cast<BoxId>(b)};
        for (CellId c : inst.constraintCells)
            result.cellLoc[c] = CellLocation{cid, -1};
    }
}

}  // namespace mss
