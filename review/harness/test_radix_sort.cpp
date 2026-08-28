#include "common.h"
#include "tests.h"

// ── 测试 7：radix_sort 正确性与稳定性 ──
void testRadixSort(Gen& g, int iter) {
    T::section("T7 radix_sort vs std::sort/stability");
    struct E {
        std::uint64_t hi, lo;
        std::uint32_t p;
    };
    for (int it = 0; it < iter; ++it) {
        const int n = 1 + g.rng.below(2000);
        std::vector<E> a(static_cast<std::size_t>(n));
        for (auto& e : a) {
            e.hi = g.rng.next() % 250;
            e.lo = g.rng.next() % 250;
            e.p = g.rng.u32();
        }
        std::vector<E> b = a;
        std::vector<E> tmp;
        radix_sort::sort(a, tmp, [](const E& e) { return e.hi; },
                         [](const E& e) { return e.lo; },
                         [](const E& e) { return e.p; });
        std::stable_sort(b.begin(), b.end(), [](const E& x, const E& y) {
            if (x.hi != y.hi) return x.hi < y.hi;
            if (x.lo != y.lo) return x.lo < y.lo;
            return x.p < y.p;
        });
        bool same = a.size() == b.size();
        for (int i = 0; i < n && same; ++i)
            if (a[static_cast<std::size_t>(i)].hi != b[static_cast<std::size_t>(i)].hi ||
                a[static_cast<std::size_t>(i)].lo != b[static_cast<std::size_t>(i)].lo ||
                a[static_cast<std::size_t>(i)].p != b[static_cast<std::size_t>(i)].p)
                same = false;
        CHECK(same, "radix_sort output != stable reference (n=%d it=%d)", n, it);
    }
}
