#include "common.h"
#include "tests.h"

static bool g_t3Dumped = false;

static void dumpT3Mismatch(int it, int s, const ObservedBoard& b,
                           const Basic::Result& basic, const Structure::Result& root,
                           const Structure::Result& full,
                           const std::vector<std::tuple<int, int, int, int>>& markFlips,
                           const std::vector<Basic::Update>& ups) {
    FILE* f = std::fopen("dump_t3_mismatch.txt", "w");
    if (!f) return;
    std::fprintf(f, "it=%d step=%d rows=%d cols=%d mines=%d\n", it, s, b.rows, b.cols,
                 b.totalMines);
    std::fprintf(f, "updates:");
    for (const auto& u : ups) {
        const auto [x, y] = b.pos(u.cell);
        std::fprintf(f, " (%d,%d)->%s", x, y,
                     u.next == Cell::Hidden ? "H" : std::to_string(static_cast<int>(u.next)).c_str());
    }
    std::fprintf(f, "\nmark flips (x,y,old->new,distToEvents):\n");
    for (const auto& [fx, fy, fo, fn] : markFlips) {
        int dist = 99;
        for (const auto& u : ups) {
            const auto [ex, ey] = b.pos(u.cell);
            dist = (std::min)(dist, (std::max)(std::abs(fx - ex), std::abs(fy - ey)));
        }
        std::fprintf(f, "  (%d,%d) %d->%d dist=%d\n", fx, fy, fo, fn, dist);
    }
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%s ", b.board[i][j] == Cell::Hidden
                                       ? "."
                                       : std::to_string(static_cast<int>(b.board[i][j])).c_str());
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "marks:\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
        std::fprintf(f, "\n");
    }
    auto dumpSide = [&](const char* tag, const Structure::Result& r) {
        std::fprintf(f, "--- %s comps=%zu ---\n", tag, r.components.size());
        for (std::size_t c = 0; c < r.components.size(); ++c) {
            const auto& inst = r.components[c];
            std::fprintf(f, "  c%zu hash=%016llx%016llx\n", c,
                         (unsigned long long)inst.shape->hash.hi,
                         (unsigned long long)inst.shape->hash.lo);
            for (std::size_t bb = 0; bb < inst.boxes.count(); ++bb) {
                std::fprintf(f, "    box%zu =", bb);
                std::vector<CellId> ids(inst.boxes.cells.begin() + inst.boxes.boxOf[bb],
                                        inst.boxes.cells.begin() + inst.boxes.boxOf[bb + 1]);
                std::sort(ids.begin(), ids.end());
                for (CellId cid : ids) {
                    const auto [x, y] = b.pos(cid);
                    std::fprintf(f, " (%d,%d)", x, y);
                }
                std::fprintf(f, "\n");
            }
            for (std::size_t ci = 0; ci < inst.shape->constraints.size(); ++ci) {
                const auto& lim = inst.shape->constraints[ci];
                std::fprintf(f, "    cons sum=%d refs=", lim.sum);
                for (BoxId bid : lim.boxIds) {
                    std::vector<CellId> ids(inst.boxes.cells.begin() +
                                                inst.boxes.boxOf[static_cast<std::size_t>(bid)],
                                            inst.boxes.cells.begin() +
                                                inst.boxes.boxOf[static_cast<std::size_t>(bid) + 1]);
                    std::sort(ids.begin(), ids.end());
                    std::fprintf(f, "{");
                    for (CellId cid : ids) {
                        const auto [x, y] = b.pos(cid);
                        std::fprintf(f, "(%d,%d)", x, y);
                    }
                    std::fprintf(f, "}");
                }
                std::fprintf(f, "\n");
            }
            std::fprintf(f, "    constraintCells:");
            for (CellId cid : inst.constraintCells) {
                const auto [x, y] = b.pos(cid);
                std::fprintf(f, " (%d,%d)", x, y);
            }
            std::fprintf(f, "\n");
        }
        std::fprintf(f, "  cellLoc:");
        for (std::size_t k = 0; k < r.cellLoc.size(); ++k) {
            const CellLocation& loc = r.cellLoc[k];
            if (loc.component != -1) std::fprintf(f, " %zu=c%d/b%d", k, loc.component, loc.box);
        }
        std::fprintf(f, "\n");
    };
    dumpSide("root(incremental)", root);
    dumpSide("full(analyze)", full);
    std::fclose(f);
    g_t3Dumped = true;
}

// ── 测试 3：Structure::Updater::update + applyDelta == 全量重析 ──
static void dumpT3Call(int id, const char* tag, const ObservedBoard& b,
                       const Basic::Result& basic, const Structure::Result& r,
                       const std::vector<Basic::Update>& ups,
                       const std::vector<std::tuple<int, int, int, int>>& flips) {
    char path[64];
    std::snprintf(path, sizeof(path), "dumpT3/%03d_%s.txt", id, tag);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols, b.totalMines);
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j) {
            const Cell c = b.board[i][j];
            std::fprintf(f, "%s ", c == Cell::Hidden ? "."
                                                      : std::to_string(static_cast<int>(c)).c_str());
        }
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "basic marks (S=0 M=1 F=2 U=3):\n");
    for (int i = 1; i <= b.rows; ++i) {
        for (int j = 1; j <= b.cols; ++j)
            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "updates(%zu):\n", ups.size());
    for (const auto& u : ups) {
        const auto [x, y] = b.pos(u.cell);
        std::fprintf(f, "  (%d,%d)->%s\n", x, y,
                     u.next == Cell::Hidden ? "H" : std::to_string(static_cast<int>(u.next)).c_str());
    }
    std::fprintf(f, "components=%zu\n", r.components.size());
    for (std::size_t c = 0; c < r.components.size(); ++c) {
        const auto& inst = r.components[c];
        std::fprintf(f, "  comp%zu hash=%016llx%016llx boxes=", c,
                     static_cast<unsigned long long>(inst.shape->hash.hi),
                     static_cast<unsigned long long>(inst.shape->hash.lo));
        for (std::size_t b2 = 0; b2 < inst.boxes.count(); ++b2) {
            std::fprintf(f, "[");
            for (std::size_t k = inst.boxes.boxOf[b2]; k < inst.boxes.boxOf[b2 + 1]; ++k) {
                const auto [x, y] = b.pos(inst.boxes.cells[k]);
                std::fprintf(f, "(%d,%d)", x, y);
            }
            std::fprintf(f, "]");
        }
        std::fprintf(f, " cons=");
        for (CellId c2 : inst.constraintCells) {
            const auto [x, y] = b.pos(c2);
            std::fprintf(f, "(%d,%d)", x, y);
        }
        std::fprintf(f, "\n");
    }
    std::fprintf(f, "flips:");
    for (const auto& [fx, fy, fo, fn] : flips) {
        int dist = 99;
        for (const auto& u : ups) {
            const auto [ex, ey] = b.pos(u.cell);
            dist = (std::min)(dist, (std::max)(std::abs(fx - ex), std::abs(fy - ey)));
        }
        std::fprintf(f, " (%d,%d)%d->%d@%d", fx, fy, fo, fn, dist);
    }
    std::fprintf(f, "\n");
    std::fclose(f);
}

static void dumpT3Delta(int id, const ObservedBoard& b, const Structure::Delta& d) {
    char path[64];
    std::snprintf(path, sizeof(path), "dumpT3/%03d_delta.txt", id);
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    std::fprintf(f, "removed:");
    for (int c : d.removed) std::fprintf(f, " %d", c);
    std::fprintf(f, "\nadded:%zu\n", d.added.size());
    for (std::size_t i = 0; i < d.added.size(); ++i)
        std::fprintf(f, "  add id=%d (data %zu)\n", d.added[i], d.addedData.size());
    std::fclose(f);
}

void testStructureUpdate(Gen& g, int iter) {
    T::section("T3 Structure update == analyze");
    // 记录第二个盘面用于重放链路
    for (int it = 0; it < iter; ++it) {
        std::printf("T3 it=%d\n", it);
        const int rows = 3 + g.rng.below(3), cols = 3 + g.rng.below(3);
        const int mines = 1 + g.rng.below(rows * cols / 2);
        ref::RefBoard rb = genConsistent(g, rows, cols, mines, cols);
        std::vector<char> trueMine(static_cast<std::size_t>(rows * cols), 0);
        for (int f : rb.trueMines) trueMine[static_cast<std::size_t>(f)] = 1;

        ObservedBoard b = toLibBoard(rb);
        Basic::Result basic = Basic::Analyzer::analyze(b);
        Structure::ShapePool pool;
        Structure::Result root = Structure::Analyzer::analyze(b, basic, pool);
        {
            // 幂等性检查：相同输入二次全量 analyze 必须一致（否则线程局部队列状态被污染）
            Structure::Result root2 = Structure::Analyzer::analyze(b, basic, pool);
            bool idem = root.components.size() == root2.components.size();
            for (std::size_t c = 0; c < root.components.size() && idem; ++c)
                if (root.components[c].shape->hash != root2.components[c].shape->hash ||
                    root.components[c].boxes.cells != root2.components[c].boxes.cells ||
                    root.components[c].constraintCells != root2.components[c].constraintCells)
                    idem = false;
            if (!idem) {
                CHECK(false, "analyze NOT idempotent it=%d (comps %zu vs %zu)", it,
                      root.components.size(), root2.components.size());
            }
            {
                char path[64];
                std::snprintf(path, sizeof(path), "dumpT3/anal_%04d.txt", it);
                FILE* f = std::fopen(path, "w");
                if (f) {
                    std::fprintf(f, "rows=%d cols=%d mines=%d\n", b.rows, b.cols,
                                 b.totalMines);
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::fprintf(f, "%s ",
                                         b.board[i][j] == Cell::Hidden
                                             ? "."
                                             : std::to_string(static_cast<int>(b.board[i][j])).c_str());
                        std::fprintf(f, "\n");
                    }
                    std::fprintf(f, "marks:\n");
                    for (int i = 1; i <= b.rows; ++i) {
                        for (int j = 1; j <= b.cols; ++j)
                            std::fprintf(f, "%d ", static_cast<int>(basic.marks[i][j]));
                        std::fprintf(f, "\n");
                    }
                    std::fprintf(f, "trueMines:");
                    for (int fz : rb.trueMines) std::fprintf(f, " %d", fz);
                    std::fprintf(f, "\ncomps=%zu\n", root.components.size());
                    for (std::size_t c = 0; c < root.components.size(); ++c) {
                        const auto& inst = root.components[c];
                        std::fprintf(f, " c%zu h=%016llx%016llx\n", c,
                                     static_cast<unsigned long long>(inst.shape->hash.hi),
                                     static_cast<unsigned long long>(inst.shape->hash.lo));
                    }
                    std::fclose(f);
                }
            }
        }
        const Basic::Result rootBasic = basic;
        const ObservedBoard rootBoard = b;

        std::vector<Structure::Delta> deltas;
        Basic::Result prevBasic = basic;
        int steps = 1 + g.rng.below(5);
        for (int s = 0; s < steps; ++s) {
            // 随机选一个非雷隐藏格揭示（T3 同 T2：只支持揭示更新）
            std::vector<std::pair<int, int>> hidden;
            for (int i = 1; i <= rows; ++i)
                for (int j = 1; j <= cols; ++j) {
                    const bool isMineCell =
                        trueMine[static_cast<std::size_t>(rb.flat(i, j))];
                    if (b.board[i][j] == Cell::Hidden && !isMineCell)
                        hidden.emplace_back(i, j);
                }
            if (hidden.empty()) { steps = s; break; }  // 无可揭示格：提前结束本盘步骤
            std::vector<Basic::Update> ups;
            {
                const auto [i, j] = hidden[g.rng.below(static_cast<int>(hidden.size()))];
                int v = 0;
                ref::forEa(i, j, rows, cols, [&](int ni, int nj) {
                    if (trueMine[static_cast<std::size_t>(rb.flat(ni, nj))]) ++v;
                });
                b.board[i][j] = toCell(v);
                ups.push_back({b.id(i, j), b.board[i][j]});
            }
            Basic::Result nextBasic = Basic::Analyzer::analyze(b);  // 全量基准
            // 诊断：记录本次更新前后 basic 标记变化（相对上一步/初始的 marks）
            std::vector<std::tuple<int, int, int, int>> markFlips;  // x,y,old,new
            {
                const Basic::Result& before = s == 0 ? basic : prevBasic;
                for (int i = 1; i <= b.rows; ++i)
                    for (int j = 1; j <= b.cols; ++j)
                        if (static_cast<int>(before.marks[i][j]) !=
                            static_cast<int>(nextBasic.marks[i][j]))
                            markFlips.emplace_back(i, j, static_cast<int>(before.marks[i][j]),
                                                   static_cast<int>(nextBasic.marks[i][j]));
            }
            prevBasic = nextBasic;
            dumpT3Call(1000 * it + s, "pre_update", b, nextBasic, root, ups, markFlips);
            Structure::Delta d =
                Structure::Updater::update(b, nextBasic, root, pool, ups);
            dumpT3Delta(1000 * it + s, b, d);
            // 注意：update 就地修改 root —— root 现在是增量后的状态
            deltas.push_back(d);

            const Structure::Result full = Structure::Analyzer::analyze(b, nextBasic, pool);
            // 语义等价比较（box 编号无关、约束顺序无关）：
            // 组件 token = box 分区（盒内 CellId 排序取集 + 全集排序）
            //           + 约束多集（(sum, 引用的 box 集列表，列表内/外均排序)）。
            // shape 哈希依赖 DFS 起点（box 首次出现顺序），增量/全量路径起点不同，
            // 哈希可能不同但内容等价，故不直接比较哈希。
            auto compToken = [&](const Structure::Instance& c) {
                std::vector<std::vector<CellId>> boxSets;
                for (std::size_t bb = 0; bb < c.boxes.count(); ++bb) {
                    std::vector<CellId> s(c.boxes.cells.begin() + c.boxes.boxOf[bb],
                                          c.boxes.cells.begin() + c.boxes.boxOf[bb + 1]);
                    std::sort(s.begin(), s.end());
                    boxSets.push_back(std::move(s));
                }
                std::sort(boxSets.begin(), boxSets.end());
                std::vector<std::pair<int, std::vector<std::vector<CellId>>>> cons;
                for (std::size_t ci = 0; ci < c.shape->constraints.size(); ++ci) {
                    const auto& lim = c.shape->constraints[ci];
                    std::vector<std::vector<CellId>> refs;
                    for (BoxId bid : lim.boxIds)
                        refs.push_back([&] {
                            std::vector<CellId> s(
                                c.boxes.cells.begin() + c.boxes.boxOf[static_cast<std::size_t>(bid)],
                                c.boxes.cells.begin() +
                                    c.boxes.boxOf[static_cast<std::size_t>(bid) + 1]);
                            std::sort(s.begin(), s.end());
                            return s;
                        }());
                    std::sort(refs.begin(), refs.end());
                    cons.emplace_back(lim.sum, std::move(refs));
                }
                std::sort(cons.begin(), cons.end());
                return std::tuple<std::vector<std::vector<CellId>>,
                                  std::vector<std::pair<int, std::vector<std::vector<CellId>>>>>(
                    std::move(boxSets), std::move(cons));
            };
            using TKey = decltype(compToken(std::declval<const Structure::Instance&>()));
            std::map<TKey, int> setA, setB;
            std::vector<TKey> keyListA, keyListB;
            for (const auto& c : root.components) { setA[compToken(c)]++; keyListA.push_back(compToken(c)); }
            for (const auto& c : full.components) { setB[compToken(c)]++; keyListB.push_back(compToken(c)); }
            bool same = (setA == setB);
            if (same) {
                // 组件内每 box 的排序格集（双份），用于 cellLoc 的 box 对齐
                auto boxSetsOf = [](const Structure::Instance& c) {
                    std::vector<std::vector<CellId>> s;
                    for (std::size_t bb = 0; bb < c.boxes.count(); ++bb) {
                        std::vector<CellId> v(c.boxes.cells.begin() + c.boxes.boxOf[bb],
                                              c.boxes.cells.begin() + c.boxes.boxOf[bb + 1]);
                        std::sort(v.begin(), v.end());
                        s.push_back(std::move(v));
                    }
                    return s;
                };
                const std::size_t cellN = static_cast<std::size_t>((rows + 1) * (cols + 1));
                for (std::size_t k = 0; k < cellN; ++k) {
                    const CellLocation& la = root.cellLoc[k];
                    const CellLocation& lb = full.cellLoc[k];
                    const bool unboundA = (la.component == -1);
                    const bool unboundB = (lb.component == -1);
                    if (unboundA || unboundB) {
                        if (unboundA != unboundB) { same = false; break; }
                        continue;
                    }
                    const TKey& ka = keyListA[static_cast<std::size_t>(la.component)];
                    const TKey& kb = keyListB[static_cast<std::size_t>(lb.component)];
                    if (ka != kb) { same = false; break; }
                    // box 对齐：按排序格集比较
                    const auto ba = boxSetsOf(root.components[static_cast<std::size_t>(la.component)]);
                    const auto bb2 = boxSetsOf(full.components[static_cast<std::size_t>(lb.component)]);
                    const bool boxUnboundA = (la.box == -1);
                    const bool boxUnboundB = (lb.box == -1);
                    if (boxUnboundA != boxUnboundB) { same = false; break; }
                    if (!boxUnboundA) {
                        if (ba[static_cast<std::size_t>(la.box)] !=
                            bb2[static_cast<std::size_t>(lb.box)])
                        { same = false; break; }
                    }
                }
            }
            CHECK(same, "structure mismatch after step %d it=%d (comps %zu vs %zu)",
                  s, it, root.components.size(), full.components.size());
            if (!same) dumpT3Mismatch(it, s, b, nextBasic, root, full, markFlips, ups);
        }

        // Delta 重放：fresh root → applyDelta 链 == 最终增量状态
        Structure::Result rp = Structure::Analyzer::analyze(rootBoard, rootBasic, pool);
        for (const auto& d : deltas) Structure::Updater::applyDelta(rp, d);
        bool same = rp.components.size() == root.components.size();
        for (int c = 0; c < static_cast<int>(root.components.size()) && same; ++c) {
            const auto& A = rp.components[static_cast<std::size_t>(c)];
            const auto& B = root.components[static_cast<std::size_t>(c)];
            if (A.shape->hash != B.shape->hash) { same = false; break; }
            if (A.constraintCells != B.constraintCells) { same = false; break; }
            if (A.boxes.cells != B.boxes.cells) { same = false; break; }
        }
        for (std::size_t k = 0; k < static_cast<std::size_t>((rows + 1) * (cols + 1)) && same; ++k)
            if (rp.cellLoc[k].component != root.cellLoc[k].component ||
                rp.cellLoc[k].box != root.cellLoc[k].box)
                same = false;
        CHECK(same, "delta replay mismatch it=%d", it);
    }
}
