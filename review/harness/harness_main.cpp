// harness_main.cpp — 测试总入口：按固定顺序调用全部套件。
// ⚠ 调用顺序与迭代数即共享 RNG（Gen g 单种子）的抽取序列，
//   任何增删/调序都会改变语料，导致回归数据不可比——改动需重录基线。
#include "common.h"
#include "tests.h"

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Gen g(0xC0FFEE12345ULL);
    std::printf("== mss review differential harness ==\n");
    testBasicAnalyzer(g, 400);
    std::printf("T1 done\n");
    testBasicUpdater(g, 200);
    std::printf("T2 done\n");
    testStructureUpdate(g, 200);
    std::printf("T3 done\n");
    testExactAnalyze(g, 500);
    std::printf("T4 done\n");
    testExactObserve(g, 300);
    std::printf("T5 done\n");
    testRadixSort(g, 60);
    std::printf("T7 done\n");
    testEndgame(g, 400);  // T8 更密集搜索（诚实概率网格）
    std::printf("T8 done\n");
    std::printf("== done: %d checks, %d fails ==\n", T::checks, T::fails);
    return T::fails == 0 ? 0 : 1;
}