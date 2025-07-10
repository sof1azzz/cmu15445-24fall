//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hyperloglog.cpp
//
// Identification: src/primer/hyperloglog.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "primer/hyperloglog.h"

namespace bustub {

/** @brief Parameterized constructor. */
template <typename KeyType>
HyperLogLog<KeyType>::HyperLogLog(int16_t n_bits)
    : cardinality_(0),
      n_bits_(n_bits > 0 ? n_bits : static_cast<int16_t>(0)),
      buckets_(1 << (n_bits > 0 ? n_bits : 0), 0ULL) {}

/**
 * @brief Function that computes binary.
 *
 * @param[in] hash
 * @returns binary of a given hash
 */
template <typename KeyType>
auto HyperLogLog<KeyType>::ComputeBinary(const hash_t &hash) const -> std::bitset<BITSET_CAPACITY> {
  return {hash};
}

/**
 * @brief Function that computes leading zeros.
 *
 * @param[in] bset - binary values of a given bitset
 * @returns leading zeros of given binary set
 */
template <typename KeyType>
auto HyperLogLog<KeyType>::PositionOfLeftmostOne(const std::bitset<BITSET_CAPACITY> &bset) const -> uint64_t {
  if (bset.none()) {
    return 0;
  }
  const uint64_t bs = bset.to_ullong();
  return __builtin_clzll(bs) + 1;
}

/**
 * @brief Adds a value into the HyperLogLog.
 *
 * @param[in] val - value that's added into hyperloglog
 */
template <typename KeyType>
auto HyperLogLog<KeyType>::AddElem(KeyType val) -> void {
  auto bit_set = ComputeBinary(CalculateHash(std::move(val)));
  auto bit_val = bit_set.to_ullong();

  auto pos = n_bits_ == 0 ? 0 : (bit_val >> (BITSET_CAPACITY - n_bits_));
  uint64_t mask = (n_bits_ == 0) ? UINT64_MAX : (1ULL << (BITSET_CAPACITY - n_bits_)) - 1;
  auto suffix = bit_val & mask;

  auto new_val = PositionOfLeftmostOne(std::bitset<BITSET_CAPACITY>{suffix}) - n_bits_;
  if (new_val > buckets_[pos]) {
    std::lock_guard<std::mutex> lock(buckets_lock_);
    buckets_[pos] = new_val;
  }
}

/**
 * @brief Function that computes cardinality.
 */
template <typename KeyType>
auto HyperLogLog<KeyType>::ComputeCardinality() -> void {
  std::lock_guard<std::mutex> lock(buckets_lock_);
  const size_t m = buckets_.size();
  const double ret = HyperLogLog::CONSTANT * m * m;
  double sum = 0.0;
  for (size_t i = 0; i < m; i++) {
    sum += pow(2.0, static_cast<int64_t>(buckets_[i]) * -1.0);
  }
  cardinality_ = std::floor(ret / sum);
}

template class HyperLogLog<int64_t>;
template class HyperLogLog<std::string>;

}  // namespace bustub
