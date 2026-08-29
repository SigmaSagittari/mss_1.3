// tests.h — 各测试套件入口声明。
// 实现分散在 review/harness/test_*.cpp（每测试一个文件）；
// harness_main.cpp 依次调用（调用顺序决定共享 RNG 的抽取序列，勿改）。
#pragma once

#include "common.h"

void testBasicAnalyzer(Gen& g, int iter);    // T1 Basic::Analyzer vs 参考标记
void testBasicUpdater(Gen& g, int iter);     // T2 Basic::Updater 增量 vs 全量
void testStructureUpdate(Gen& g, int iter);  // T3 Structure::Updater == analyze
void testExactAnalyze(Gen& g, int iter);     // T4 Exact::analyze vs 参考枚举
void testExactObserve(Gen& g, int iter);     // T5 Exact::observe vs 参考枚举
void testEndgame(Gen& g, int iter);          // T8 EndgameBruteforce vs 朴素最优解
void testRadixSort(Gen& g, int iter);        // T7 radix_sort 正确性与稳定性
void testDeltaReverse(Gen& g, int iter);     // T9 Delta apply/unapply 往返（树状路径游走）
void testFrontierCells(Gen& g, int iter);    // T10 frontierCells == 暴力扫描