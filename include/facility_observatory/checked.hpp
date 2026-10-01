// Facility Observatory - checked, total integer arithmetic.
//
// Every helper here is a total function: it either produces the exact result
// and returns true, or reports that the exact result is not representable and
// returns false. No helper can invoke undefined behaviour, and none of them
// ever saturates silently.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_CHECKED_HPP
#define FACILITY_OBSERVATORY_CHECKED_HPP

#include <cstddef>
#include <cstdint>
#include <limits>

namespace fo {

// Absolute value of a signed 64-bit integer as an unsigned magnitude. Defined
// for INT64_MIN, where the signed negation would overflow.
[[nodiscard]] constexpr std::uint64_t magnitude(std::int64_t value) noexcept {
  if (value < 0) {
    return static_cast<std::uint64_t>(0) - static_cast<std::uint64_t>(value);
  }
  return static_cast<std::uint64_t>(value);
}

// Returns true and writes the exact sum, or false when |a + b| > INT64_MAX.
[[nodiscard]] constexpr bool checked_add(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b > 0 && a > std::numeric_limits<std::int64_t>::max() - b) {
    return false;
  }
  if (b < 0 && a < std::numeric_limits<std::int64_t>::min() - b) {
    return false;
  }
  out = a + b;
  return true;
}

// Returns true and writes the exact difference, or false on overflow.
[[nodiscard]] constexpr bool checked_sub(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b == std::numeric_limits<std::int64_t>::min()) {
    if (a >= 0) {
      return false;
    }
    out = a - b;
    return true;
  }
  return checked_add(a, -b, out);
}

// Returns true and writes the exact product, or false on overflow. Implemented
// entirely in unsigned arithmetic so that no intermediate is ever undefined.
[[nodiscard]] constexpr bool checked_mul(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  const bool negative = (a < 0) != (b < 0);
  const std::uint64_t ua = magnitude(a);
  const std::uint64_t ub = magnitude(b);
  if (ub != 0 && ua > std::numeric_limits<std::uint64_t>::max() / ub) {
    return false;
  }
  const std::uint64_t product = ua * ub;
  constexpr std::uint64_t kInt64MinMagnitude = static_cast<std::uint64_t>(1) << 63;
  if (negative) {
    if (product > kInt64MinMagnitude) {
      return false;
    }
    if (product == kInt64MinMagnitude) {
      out = std::numeric_limits<std::int64_t>::min();
      return true;
    }
    out = -static_cast<std::int64_t>(product);
    return true;
  }
  if (product > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  out = static_cast<std::int64_t>(product);
  return true;
}

// Returns true and writes the exact quotient (truncating toward zero), or false
// when the divisor is zero or the quotient is not representable (INT64_MIN / -1).
[[nodiscard]] constexpr bool checked_div(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b == 0) {
    return false;
  }
  if (a == std::numeric_limits<std::int64_t>::min() && b == -1) {
    return false;
  }
  out = a / b;
  return true;
}

// Non-negative remainder with the sign of the dividend, undefined only for b == 0.
[[nodiscard]] constexpr bool checked_mod(std::int64_t a, std::int64_t b, std::int64_t& out) noexcept {
  if (b == 0) {
    return false;
  }
  if (b == -1) {
    out = 0;
    return true;
  }
  out = a % b;
  return true;
}

[[nodiscard]] constexpr bool checked_add_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_mul_u64(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
  if (b != 0 && a > std::numeric_limits<std::uint64_t>::max() / b) {
    return false;
  }
  out = a * b;
  return true;
}

[[nodiscard]] constexpr bool checked_add_size(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (a > std::numeric_limits<std::size_t>::max() - b) {
    return false;
  }
  out = a + b;
  return true;
}

[[nodiscard]] constexpr bool checked_mul_size(std::size_t a, std::size_t b, std::size_t& out) noexcept {
  if (b != 0 && a > std::numeric_limits<std::size_t>::max() / b) {
    return false;
  }
  out = a * b;
  return true;
}

// Narrowing cast that is checked at run time; returns false when the value does
// not round-trip. Used for every width reduction in the persistence codec.
[[nodiscard]] constexpr bool checked_narrow_u64(std::uint64_t value, std::uint32_t& out) noexcept {
  if (value > std::numeric_limits<std::uint32_t>::max()) {
    return false;
  }
  out = static_cast<std::uint32_t>(value);
  return true;
}

[[nodiscard]] constexpr bool checked_narrow_size(std::uint64_t value, std::size_t& out) noexcept {
  if (value > std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  out = static_cast<std::size_t>(value);
  return true;
}

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_CHECKED_HPP
