// harness_main.cpp — 测试总入口：按固定顺序调用全部套件。
// ⚠ 调用顺序与迭代数即共享 RNG（Gen g 单种子）的抽取序列，
//   任何增删/调序都会改变语料，导致回归数据不可比——改动需重录基线。
// 计时：每节输出耗时（秒），总用时最后汇总——基线记录在注释末尾。
#include <chrono>
#include "common.h"
#include "tests.h"

namespace {
// 单调时钟秒数（计时用，无关语料）。
double nowSec() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}
}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    Gen g(0xC0FFEE12345ULL);
    std::printf("== mss review differential harness ==\n");
    const double tTot0 = nowSec();

    double t0 = nowSec();
    testBasicAnalyzer(g, 40000);
    std::printf("T1 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testBasicUpdater(g, 20000);
    std::printf("T2 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testStructureUpdate(g, 20000);  // T3 是结构增量核心：量级对标 0.04%/盘 的隐藏触发率
    std::printf("T3 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testExactAnalyze(g, 30000);
    std::printf("T4 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testExactObserve(g, 20000);
    std::printf("T5 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testRadixSort(g, 6000);
    std::printf("T7 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testEndgame(g, 40000);  // T8 更密集搜索（诚实概率网格）
    std::printf("T8 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testDeltaReverse(g, 20000);  // T9 Delta apply/unapply 往返（树状路径游走）
    std::printf("T9 done (%.2fs)\n", nowSec() - t0);

    t0 = nowSec();
    testFrontierCells(g, 20000);  // T10 frontierCells == 暴力扫描
    std::printf("T10 done (%.2fs)\n", nowSec() - t0);

    const double total = nowSec() - tTot0;
    std::printf("== done: %d checks, %d fails, %.2fs total ==\n", T::checks, T::fails,
                total);
    return T::fails == 0 ? 0 : 1;
}

// ── 基线记录（语料 = 可移植洗牌，seed 0xC0FFEE12345）──
//   盘面数：T1 40000 / T2 20000 / T3 20000 / T4 30000 / T5 20000 / T7 6000 / T8 40000 / T9 20000 / T10 20000
//   实测（T9/T10 加入后 g++ 重录；T1–T8 语料序列未变）：
//     g++        -O1      : 1,792,156 checks / 0 fails / 5.81s 总耗时
//     MSVC Release x64    : 待重录
//     MSVC Debug   x64    : 待重录
//   语料可移植（同种子跨编译器逐位一致）。改盘面数/顺序/种子即失效，需重录。