// radix_stability.cpp — 验证 radix_sort 的稳定性声明（重复键、小数组分支）
#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>
#include <algorithm>
#include "core/utility/radix_sort.h"

struct E {
    std::uint64_t hi, lo;
    std::uint32_t seq;  // 唯一序号，用于检测同键组的相对顺序保持
};

int main() {
    std::mt19937_64 rng(42);
    int unstable = 0, total = 0;
    for (int trial = 0; trial < 200; ++trial) {
        const int n = 1 + static_cast<int>(rng() % 2000);
        std::vector<E> a(static_cast<std::size_t>(n));
        for (auto& e : a) {
            e.hi = rng() % (trial % 3 == 0 ? 2 : 17);   // 制造重复键
            e.lo = rng() % (trial % 3 == 0 ? 2 : 17);
            e.seq = static_cast<std::uint32_t>(trial * 10000 + (&e - &a[0]));
        }
        // 期望：同 (hi,lo) 组内按原出现顺序（稳定）
        std::vector<E> ref = a;
        std::stable_sort(ref.begin(), ref.end(), [](const E& x, const E& y) {
            return x.hi != y.hi ? x.hi < y.hi : x.lo < y.lo;
        });
        std::vector<E> out = a;
        std::vector<E> tmp;
        // 注意：radix_sort 的通用接口是按“访问器”排序；这里只排 (hi,lo)，
        // 不排 seq —— 正是稳定性测试的形态。
        mss::radix_sort::sort(out, tmp, [](const E& e) { return e.hi; },
                         [](const E& e) { return e.lo; });
        // 同键组内应保持输入顺序（按 seq 递增）
        bool ok = true;
        for (int i = 0; i < n; ++i) {
            if (ref[i].hi != out[i].hi || ref[i].lo != out[i].lo) { ok = false; break; }
            // 组内顺序校验：模拟稳定排序的结果，只需比较相邻同键元素在原数组中位置
        }
        // 更强校验：直接比较 out 与“以输入下标为第三键的稳定序”
        std::vector<E> stableRef = a;
        std::stable_sort(stableRef.begin(), stableRef.end(), [](const E& x, const E& y) {
            return x.hi != y.hi ? x.hi < y.hi : x.lo < y.lo;
        });
        for (int i = 0; i < n; ++i)
            if (stableRef[static_cast<std::size_t>(i)].seq !=
                out[static_cast<std::size_t>(i)].seq) { ok = false; break; }
        ++total;
        if (!ok) {
            ++unstable;
            if (unstable <= 3)
                std::printf("trial %d n=%d: radix output NOT stable\n", trial, n);
        }
    }
    std::printf("total=%d unstable=%d\n", total, unstable);
    return unstable == 0 ? 0 : 1;
}