#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <vector>

namespace mss {

// 固定 64 个权重 bucket 的稠密优先集合。boxId 必须在 reset(boxCount) 给出的
// 范围内，weight 是 [0, kWeightCount) 内的 bucket 下标。每个 box 至多出现一次。
// buckets_ 用连续数组存 box；bucketOf_/indexInBucket_ 让删除任意 box 只需 swap-pop。
class BitFlagSet {
public:
    static constexpr int kWeightCount = 64;

    void reset(int boxCount) {
        for (auto& bucket : buckets_) bucket.clear();
        bucketOf_.assign(boxCount, -1);
        indexInBucket_.resize(boxCount);
        nonempty_ = 0;
    }

    bool empty() const { return nonempty_ == 0; }

    bool contains(int boxId) const { return bucketOf_[boxId] != -1; }

    void insert(int boxId, int weight) {
        std::vector<int>& bucket = buckets_[weight];
        if (bucket.empty()) nonempty_ |= bit(weight);
        bucketOf_[boxId] = weight;
        indexInBucket_[boxId] = static_cast<int>(bucket.size());
        bucket.push_back(boxId);
    }

    void erase(int boxId) {
        const int weight = bucketOf_[boxId];
        std::vector<int>& bucket = buckets_[weight];
        const int index = indexInBucket_[boxId];
        const int last = bucket.back();
        bucket[index] = last;
        indexInBucket_[last] = index;
        bucket.pop_back();
        bucketOf_[boxId] = -1;
        if (bucket.empty()) nonempty_ &= ~bit(weight);
    }

    // 返回最小 weight bucket 中任意一个 box，并将它删除。调用方保证非空。
    int popFirst() {
        const int weight = static_cast<int>(std::countr_zero(nonempty_));
        const int boxId = buckets_[weight].back();
        erase(boxId);
        return boxId;
    }

private:
    static std::uint64_t bit(int weight) { return 1ULL << weight; }

    std::array<std::vector<int>, kWeightCount> buckets_;
    std::vector<int> bucketOf_;
    std::vector<int> indexInBucket_;
    std::uint64_t nonempty_ = 0;
};

}  // namespace mss
