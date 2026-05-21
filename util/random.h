//
// Modernized for C++23.
//
// Simple random number generator for internal use (testing, skip list heights,
// etc.).
//
// C++23 changes:
//   - Old: hand-rolled LCG (linear congruential generator)
//   - New: std::mt19937 engine + std::uniform_int_distribution
//   - Better randomness quality, no manual modular arithmetic
//   - Parameter types int → uint32_t (avoid signed issues)

#pragma once

#include <cstdint>
#include <random>

namespace leveldb {

class Random {
 public:
  explicit Random(uint32_t seed)
      : engine_(seed == 0 ? 1u : seed) {}

  // Returns a pseudo-random 32-bit unsigned integer.
  uint32_t Next() { return dist_(engine_); }

  // Returns a uniformly distributed value in [0, n).
  // REQUIRES: n > 0
  uint32_t Uniform(uint32_t n) {
    return std::uniform_int_distribution<uint32_t>(0, n - 1)(engine_);
  }

  // Returns true approximately 1/n of the time.
  // REQUIRES: n > 0
  bool OneIn(uint32_t n) { return Uniform(n) == 0; }

  // Pick "base" uniformly from [0, max_log] and then return "base" random
  // bits. The effect is to pick a number in [0, 2^max_log - 1] with
  // exponential bias towards smaller numbers.
  uint32_t Skewed(int max_log) {
    return Uniform(1u << Uniform(static_cast<uint32_t>(max_log + 1)));
  }

 private:
  std::mt19937 engine_;
  std::uniform_int_distribution<uint32_t> dist_;
};

}  // namespace leveldb
