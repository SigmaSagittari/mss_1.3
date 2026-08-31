# LongTermRiskHelper 移植设计（JS → C++）

> 目标：把 ref/jsminesweeper 的 LongTermRiskHelper.js（及 Old 变体）的核心能力移植到
> mss/src 的 C++ 分析管线，与 basic / structure / distribution / exact 对齐。
> 状态：设计稿（尚未实现）。

## 1. 参考实现是什么

两份文件：
- `Minesweeper/client/LongTermRiskHelper.js`（现行版，solver_main.js 实际使用）
- `Minesweeper/client/LongTermRiskHelperOld.js`（旧版：fifty-percenters 配对 + 长期安全度）

### 1.1 现行版：50/50 "influence"（影响度）

核心概念：某些隐藏格虽然单看雷概率不高，但点击后会**必然把局面逼进 50/50 赌局**
（长期风险）。helper 扫描全盘找"候选 50/50 结构"，对每个结构用
`solver.countSolutions(board, notMines)`（先把一组格标记为雷、另一组标记为安全，
再数全盘解数）求"该结构成立"的方案数 N：

**2-tile 50/50（横/竖相邻对，各 6 个外围源格）**
- 条件：两个格都隐藏且非确定雷；6 个外围格中无已揭示格（否则信息不封闭 → 放弃），
  已知雷（basic Mine 或 PE 新雷）剔除出源列表；
- 计数：`mines = missingMines ∪ {t1}，safes = {t2}`，数解 N；
- 若 N > 0：该对在 N 条方案里是"t1 必雷、t2 必空"的强制 50/50。
  `influence[i] += N`（阈值 0.025 之上才记），missingMines 作为 "enabler" 也记 N。

**4-tile（2×2 块）50/50（4 个对角源格）**
- 条件：4 格全部隐藏；4 个对角源格无揭示；`mines = missingMines ∪ {t1, t4}，
  safes = {t2, t3}`；同样计数 N（maxMissingMines = 4，即 missingMines ≤ 2）。

**pseudo-5050 判定**：若某个格所属 box 的 `mineTally == influenceTally`
（该格的整盒雷迹完全被 50/50 结构覆盖），该格进 `pseudos`——表示它是
"不可避免的 50/50"（或安全），solver_main 用它提前结束正常搜索、直接推荐点它。

**单格聚合 `findTileInfluence(tile)`**：tile 参与的 ≤4 个 2-tile 对的和 +
≤4 个 2×2 块的 max + enabler 贡献，然后钳制到
`min(mineTally, total − mineTally)`（P(50/50)/2 不可能超过 P(雷) 或 P(安全)）。

**`getInfluencedTiles(threshold)`**：返回"安全额 = total − mineTally + influence
超过 total×threshold、且非死格、且 influence ≠ 0"的格——即 50/50 影响改善过的候选。
solver_main 用它给候选加权：`fiftyFiftyInfluence = 1 + (influence / safetyTally) × 0.9`。

### 1.2 旧版：长期安全度（已被现行版取代，价值低）

把 PE 的 50% 概率格（getFiftyPercenters）两两配对成 2-tile 风险，检测"仅剩 1 个
信息源（poi）"的封闭 50/50，计算 `safety = 1 − P(poi 是雷)×0.5`，
长期安全度 = 各风险连乘。还提供 get5050Breakers（风险周边可破局格）。
移植优先级低：现行版 influence 机制已覆盖其语义（且不用依赖 PE 的 50% 格）。

## 2. JS → C++ 概念映射

| JS 概念 | C++ 现有对应 |
|---|---|
| board / getTileXY / width·height | `ObservedBoard`（board Grid<Cell>，id/pos） |
| tile.isCovered() | `board.board[x][y] == Cell::Hidden` |
| tile.isSolverFoundBomb() | `basic.marks[x][y] == Mark::Mine` |
| isHidden = covered ∧ ¬确定雷 | Hidden ∧ marks ≠ Mine（注意 Frontier 也算 hidden） |
| setFoundBomb / unsetFoundBomb + countSolutions(board, notMines) | **无现成 API → 需新增 `countWithForces`**（见 §3） |
| pe.finalSolutionsCount / totalSolutions | `Probability::Result::candidates`（long double） |
| pe.getBox(tile).mineTally / offEdgeMineTally | 统一公式：`mineTally(cell) = mineProbability(cell) × candidates`（对 box 格与 Unknown 格都成立；off-edge 即 tCellProbability×candidates） |
| pe.isNewMine(tile)（PE 推出的雷） | `mineProbability(cell) ≥ 1 − eps`（boxProbs == 1 的 box 成员） |
| pe.isDead(tile) | 无现成缓存 → 两种方案：(a) 调用方传入死格集合（midgame_search 的 observe 单结局判定）；(b) 启发式：box 大小 == 1 且非确定雷（≈ JS lonely tile）。推荐 (a)，默认空集合 |
| BigInt 精确方案数 | long double（exact.h 既有口径，combLog 对数域）；所有 `==` 比较改相对 epsilon |
| minesLeft | `board.totalMines − basic.mineSum`（exact.h 里的 M） |
| 方案数相等判断（pseudo 用的 `influenceTally == mineTally`） | 相对 epsilon（如 1e−9），见 §4 |

## 3. 关键缺口：countSolutions 的 C++ 等价物

JS 每候选对都调一次全盘解数。C++ 管线没有"强制雷/强制安全"计数 API，但可以
零新增算法地复现——**临时 mark 世界 + 既有管线**：

```cpp
// countWithForces(board, basic, shapePool, distPool, forcedMines, forcedSafes) → long double
// 1. 拷贝 basic.Result（Grid<Mark> 很小）
// 2. forcedMines → Mine；forcedSafes → Safe
// 3. Structure::Analyzer::analyze(board, marksCopy, shapePool)   // 全新建构
// 4. return Exact::analyze(board, marksCopy, structure', distPool).candidates;
//    （Exact::analyze 的 M = totalMines − mineSum、tSum = unknownSum 都由 marks 派生，
//      强制雷吃掉雷预算、强制安全退出 T 池——语义自动正确）
```

正确性论证：管线的一切（T 池大小、剩余雷数、box 归属、约束 sum）都只从
marks 派生，把格改成 Mine/Safe 等价于"该格不参与放置"，与 JS
setFoundBomb/unsetFoundBomb + notMines 的语义一致。JS 改的是可变 tile 状态然后
还原；C++ 拷贝一份 marks 再丢弃，无需回滚，天然无副作用。

- 成本：每候选对 = 1 次 O(board) 结构重构 + 1 次 Exact::analyze。
  2-tile 候选的强制格 ≤ 3（missingMines ≤ 2 + t1），2×2 块 ≤ 4（含 t1+t4；
  maxMissingMines=4 → missingMines ≤ 2）。候选对数量级：前沿邻接对几十~几百，
  远小于 JS 版（每对一次 BigInt 全盘计数）。
- **优化（后续再做）**：计数可以只重算"受影响连通块"——强制格只改变其所在
  组件（≤3 个）与 T 池；其余组件的分布已池缓存，联合多项式 pRest 可复算复用：
  `count = Σ_{受影响组件分配 a} ways_new(a) × denominator(pRest, M − mines(a), tPoolNew)`
  其中 `tPoolNew = unknownSum − |(F∪S) ∩ Unknown|`。第一版先做全管线版，跑通后
  再换局部重算（差分对拍保证一致）。

## 4. 必须处理的精度问题（移植的大坑）

exact.h 的方案数是对数域 long double（`combLog`），不是 JS 的 BigInt：

1. `influenceTally == mineTally`（pseudo 判定）、`influence == 0`、`== total`
   ——一律改相对比较：`|a−b| ≤ eps·max(a,b)`，eps ≈ 1e−9（对数域误差是相对的，
   计数上万时 1e−9 宽松有余；计数很小时绝对误差也极小）。pseudo 判定本来就是
   启发式（只影响落子偏好，不影响正确性）。
2. `isNewMine`：boxProbs == 1 的盒成员在 fp 下可能 0.9999…——用 `≥ 1 − eps`。
3. JS `divideBigInt(a, b, 5)`（5 位定点）→ 直接 long double 除法，精度更好，
   阈值语义不变（0.025 与 0.9 等魔法数原样保留）。
4. 不变量校验：Σ digit + explosion = 1 之类的闭合检查沿用 exact.h 手法，
   测试里以相对误差 1e−12 断言。

## 5. 移植时顺手修掉的 JS 缺陷（对拍时要注意）

1. `getBoxInfluence` 用了全局 `board.height`（应为 this.board）——靠浏览器全局
   才没炸。移植版用 this。
2. `getMissingMines` 的 y 界检查 `loc.getY() >= this.board.getHeight` 是
   getHeight 方法名 vs 属性，永远不成立 → y 越界检查实际失效。移植版补正确的
   双坐标界检查。
3. **横向 ratio ×2、纵向 ×1 的不对称**（checkFor2Tile5050：horizontal 的
   influenceRatio 乘了 2，vertical 没乘）——疑似参考实现 bug，影响阈值门控与
   enabler 记账。移植版保留该不对称（参数化 pairRatioScale，默认照抄参考行为），
   差分对拍才能一致；注释里标明可疑。
4. 打印信息里的 "chance of being 50/50" 是 ratio×2 的展示值，**与存储的 tally
   （单向计数）不同**——移植版只保留 tally 语义，诊断打印可选。
5. 2-tile 的 maxMissingMines=2 实际只允许 1 个 missing mine（`len+1 > 2`），
   注释却写 "less than 3"——参数化即可，无需对齐注释。

## 6. 建议的 C++ 形态

新文件 `mss/src/analysis/long_term_risk.h`（header-only，house style，thread_local
scratch，池由调用方持有传入），注册进 mss.vcxproj 与 filters。

```cpp
namespace mss {

// 长期风险：50/50 influence 扫描（JS LongTermRiskHelper 的移植）。
struct LongTermRisk {
    struct Config {
        long double influenceThreshold = 0.025L;
        int maxMissingMines2 = 2;      // 2-tile（实际允许 len+1 <= max）
        int maxMissingMines4 = 4;      // 2x2 box
        long double eqEps = 1e-9L;     // 计数相等的相对容差（BigInt → long double）
    };

    struct Result {
        // influence 数组按 CellId 索引，单位 = 方案数（long double）。
        // influenceEnablers：enabler 贡献；pseudos：伪 50/50 格。
        std::vector<long double> influence5050s;    // size = (rows+1)*(cols+1)
        std::vector<long double> influenceEnablers;
        std::vector<CellId> pseudos;

        // findTileInfluence：单格聚合（2-tile 和 + 2x2 max + enabler，钳制到
        //   min(mineTally, total − mineTally)）。
        long double findTileInfluence(CellId cell, const Probability::Result& prob) const;
        // getInfluencedTiles：安全额 = total − mineTally + influence 超过
        //   total×threshold 且非死格且 influence≠0 的格。
        std::vector<CellId> getInfluencedTiles(long double threshold,
                                               const Probability::Result& prob,
                                               const Basic::Result& basic,
                                               const Structure::Result& structure,
                                               const std::vector<CellId>& dead) const;
    };

    // 全盘扫描：2-tile 横竖 + 2x2 box → 填 influence 数组 + pseudos。
    // dead：调用方给的可选死格集合（midgame_search 单结局判定），空 = 无死格。
    static Result findInfluence(const ObservedBoard& board, const Basic::Result& basic,
                                const Structure::Result& structure,
                                const Probability::Result& prob,
                                Structure::ShapePool& shapePool,
                                Distribution::DistPool& distPool,
                                const std::vector<CellId>& dead = {},
                                const Config& cfg = Config{});

    // §3 的计数原语（public，便于测试）：强制一组雷与一组安全后的全盘解数。
    static long double countWithForces(const ObservedBoard& board,
                                       const Basic::Result& basic,
                                       Structure::ShapePool& shapePool,
                                       Distribution::DistPool& distPool,
                                       const std::vector<CellId>& forcedMines,
                                       const std::vector<CellId>& forcedSafes);
private:
    // 候选对扫描主体（checkFor2Tile5050 / checkForBox5050 / getHorizontal /
    // getVertical / getBoxInfluence / getMissingMines / isHidden / addInfluence 的
    // 静态化实现），全部 thread_local 复用缓冲区。
};

}  // namespace mss
```

循环结构照抄参考：横竖 2-tile 各 (w−1)×h / w×(h−1) 个起点，2×2 块 (w−1)×(h−1) 个
起点；找到 pseudo 提前终止（参考行为：findInfluence 在两阶段之间提前 return）。
getMissingMines 的源格集合：2-tile 横 = 上排 3 + 下排 3；竖 = 左列 3 + 右列 3；
box = 4 个对角。

## 7. 集成点

- **midgame_search**：docs v2~v6 一直挂着的"50/50 结构检测：暂缓"就是它。
  两个入口：
  1. 无安全格时先跑 `findInfluence`，pseudos 非空 → 直接推荐 pseudo 格
     （对应 solver_main L853-877 的 early-exit）；
  2. 候选排序加权：`weight = (blendedSafety + prob × fiftyFiftyInfluence) × bonus`，
     `fiftyFiftyInfluence = 1 + (findTileInfluence / safetyTally) × 0.9`
     （对应 solver_main L1596-1632；safetyTally = candidates − mineTally）。
- **EndgameBruteforce / exact**：不感知，无改动。
- 旧版 getLongTermSafety / get5050Breakers：暂不移植（现行版语义已覆盖），
  若日后要做"整局存活率"指标再补。

## 8. 测试与差分验证

沿用 review/harness 模式：
- 新增 `review/harness/test_long_term_risk.cpp`，套件**追加在 harness_main 末尾**
  （改顺序会破坏共享 RNG 语料基线；末尾追加 + 重录基线）。
- 手工小盘差分：1×2 / 2×2 / 2×3 / 3×3 边界盘，手算 influence 值与 pseudo 集合；
  对称性不变量（左右/上下镜像盘 influence 镜像相等）。
- 跨层不变量：`mineTally = mineProbability × candidates` 与 exact.h 自洽；
  `countWithForces(∅, ∅) == candidates`（空强制 = 恒等）；
  `countWithForces(F, S)` 与"先 applyDelta 改 marks 再 Exact::analyze"结果一致。
- JS 对拍：ref/headless_solver.js 已能把 LongTermRiskHelper.js 拼进无头运行；
  对相同盘面 dump 参考的 influence/pseudos（或至少 2-tile/box 候选 N 值），
  与 C++ 版逐盘对照（long double vs BigInt 用相对误差容差）。
