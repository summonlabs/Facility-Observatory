// Facility Observatory - observation/publication time model.
//
// Time is always an explicit input. Nothing in this library reads a wall clock
// on its own, so every result is reproducible from the evidence plus the
// evaluation instant the caller supplied.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_TIME_HPP
#define FACILITY_OBSERVATORY_TIME_HPP

#include <chrono>
#include <compare>
#include <cstdint>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>

#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"

namespace fo {

using Duration = std::chrono::nanoseconds;
using UnixNanos = std::int64_t;

// A UTC instant. Unset is a first-class state: it is ordered before every real
// instant and is never accepted as an observation or publication time.
class FO_API Timestamp {
 public:
  static constexpr UnixNanos kUnsetValue = std::numeric_limits<UnixNanos>::min();

  constexpr Timestamp() noexcept = default;

  static Outcome<Timestamp> from_unix_nanos(UnixNanos nanos);

  // Strict RFC 3339: YYYY-MM-DDTHH:MM:SS[.fraction](Z|+HH:MM|-HH:MM).
  // Leap seconds are rejected rather than silently folded into the next second.
  static Outcome<Timestamp> parse_rfc3339(std::string_view text);

  [[nodiscard]] constexpr bool is_set() const noexcept { return nanos_ != kUnsetValue; }
  [[nodiscard]] constexpr UnixNanos unix_nanos() const noexcept { return nanos_; }

  // Canonical rendering: shortest of 3, 6 or 9 fractional digits that is exact,
  // always suffixed with 'Z'. An unset timestamp renders as "unset".
  [[nodiscard]] std::string to_rfc3339() const;

  friend constexpr bool operator==(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ == rhs.nanos_; }
  friend constexpr auto operator<=>(Timestamp lhs, Timestamp rhs) noexcept { return lhs.nanos_ <=> rhs.nanos_; }

 private:
  explicit constexpr Timestamp(UnixNanos nanos) noexcept : nanos_(nanos) {}
  UnixNanos nanos_{kUnsetValue};
};

// Duration arithmetic that saturates rather than wrapping.
FO_API Duration saturating_add(Duration lhs, Duration rhs) noexcept;
FO_API Duration saturating_sub(Duration lhs, Duration rhs) noexcept;

// Exact elapsed time; fails when either stamp is unset.
FO_API Outcome<Duration> duration_between(Timestamp later, Timestamp earlier);

// Compact duration literal: 250ms, 30s, 5m, 2h, 1d.
FO_API Outcome<Duration> parse_duration(std::string_view text);
FO_API std::string to_string(Duration duration);

// ---------------------------------------------------------------------------
// Clocks
// ---------------------------------------------------------------------------
class FO_API Clock {
 public:
  Clock() = default;
  virtual ~Clock() = default;
  Clock(const Clock&) = delete;
  Clock& operator=(const Clock&) = delete;
  Clock(Clock&&) = delete;
  Clock& operator=(Clock&&) = delete;

  [[nodiscard]] virtual Timestamp now() const = 0;
};

class FO_API SystemClock final : public Clock {
 public:
  [[nodiscard]] Timestamp now() const override;
};

// Deterministic clock for tests and replay. Never moves backwards.
class FO_API ManualClock final : public Clock {
 public:
  explicit ManualClock(Timestamp start);

  [[nodiscard]] Timestamp now() const override;
  void set(Timestamp value) noexcept;
  Outcome<Duration> advance(Duration delta) noexcept;

 private:
  mutable std::mutex mutex_;
  Timestamp now_;
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_TIME_HPP
