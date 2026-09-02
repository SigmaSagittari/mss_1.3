#include <chrono>

#include "test/common.h"
#include "test/basic/flood_probability.h"
#include "test/distribution/greedy_order.h"
#include "test/performance/real_game_probability.h"
#include "test/spread/probability.h"

int main() {
    using namespace mss::test;
    constexpr TestConfig normal_test{
        .rows = 30, .cols = 30, .mines = 225,
        .firstMoveSafe = true,
    };

    Rng rng(0xC0FFEE12345ULL);
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + std::chrono::seconds(20);
    long long positions = 0;
    long long games = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ++games;
        generateGame(normal_test, rng, [&](const Snapshot&) {
            if (std::chrono::steady_clock::now() < deadline) ++positions;
        });
    }
    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::cout << "performance/graph-real-game: " << positions << " positions from "
              << games << " games, " << seconds << "s, " << positions / seconds
              << " positions/s\n";
    return 0;
}
