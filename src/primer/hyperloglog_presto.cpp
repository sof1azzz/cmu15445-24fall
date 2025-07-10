//===----------------------------------------------------------------------===//
//
//                         BusTub
//
// hyperloglog_presto.cpp
//
// Identification: src/primer/hyperloglog_presto.cpp
//
// Copyright (c) 2015-2025, Carnegie Mellon University Database Group
//
//===----------------------------------------------------------------------===//

#include "primer/hyperloglog_presto.h"

namespace bustub {

template <typename KeyType>
HyperLogLogPresto<KeyType>::HyperLogLogPresto(int16_t n_leading_bits)
    : dense_bucket_((1 << ((n_leading_bits > 0 ? n_leading_bits : 0))), std::bitset<DENSE_BUCKET_SIZE>()),
      cardinality_(0),
      leading_bits_((n_leading_bits > 0 ? n_leading_bits : 0)) {}

template <typename KeyType>
auto HyperLogLogPresto<KeyType>::AddElem(KeyType val) -> void {
  /** @TODO(student) Implement this function! */
  hash_t h = CalculateHash(std::move(val));
  int r = 0;
  for (int i = 0; i < leading_bits_; i++) {
    r += (1 << (leading_bits_ - i - 1)) * ((h >> (64 - i - 1)) & 1);
  }
  int continuous_zero = 0;
  for (uint64_t i = 0; i < 64 - static_cast<uint64_t>(leading_bits_); i++) {
    if (((h >> i) & 1) != 0) {
      break;
    }
    continuous_zero++;
  }
  int overflow_bucket_index = overflow_bucket_[r].to_ulong();
  int dense_bucket_index = dense_bucket_[r].to_ulong();
  if ((overflow_bucket_index << DENSE_BUCKET_SIZE) + dense_bucket_index < continuous_zero) {
    overflow_bucket_index = continuous_zero >> DENSE_BUCKET_SIZE;
    dense_bucket_index = (continuous_zero % (1 << DENSE_BUCKET_SIZE));
    overflow_bucket_[r] = overflow_bucket_index;
    dense_bucket_[r] = dense_bucket_index;
  }
}

template <typename T>
auto HyperLogLogPresto<T>::ComputeCardinality() -> void {
  /** @TODO(student) Implement this function! */
  double avg = 0;
  uint64_t m = (1 << leading_bits_);
  for (uint64_t i = 0; i < m; i++) {
    int num = (overflow_bucket_[i].to_ulong() << DENSE_BUCKET_SIZE) + dense_bucket_[i].to_ulong();
    avg += pow(2, -num);
  }
  avg = m / avg;
  cardinality_ = std::floor(CONSTANT * m * avg);
}

template class HyperLogLogPresto<int64_t>;
template class HyperLogLogPresto<std::string>;
}  // namespace bustub