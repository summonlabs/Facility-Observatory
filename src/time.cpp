// Facility Observatory - observation/publication time model.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/time.hpp"

#include <cstddef>
#include <string>

#include "facility_observatory/checked.hpp"

namespace fo {
namespace {

constexpr std::int64_t kNanosPerSecond = 1000000000LL;
constexpr std::int64_t kNanosPerMinute = 60LL * kNanosPerSecond;
constexpr std::int64_t kNanosPerHour = 60LL * kNanosPerMinute;
constexpr std::int64_t kNanosPerDay = 24LL * kNanosPerHour;
constexpr std::int64_t kSecondsPerDay = 86400LL;

// Days from 1970-01-01 to y-m-d, proleptic Gregorian. Howard Hinnant's
// algorithm; valid for the whole int64 nanosecond range.
constexpr std::int64_t days_from_civil(std::int64_t year, unsigned month, unsigned day) noexcept {
  year -= month <= 2U ? 1 : 0;
  const std::int64_t era = (year >= 0 ? year : year - 399) / 400;
  const unsigned year_of_era = static_cast<unsigned>(year - era * 400);
  const int month_adjusted = static_cast<int>(month) + (month > 2U ? -3 : 9);
  const unsigned day_of_year =
      (153U * static_cast<unsigned>(month_adjusted) + 2U) / 5U + day - 1U;
  const unsigned day_of_era =
      year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
  return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

constexpr void civil_from_days(std::int64_t days, std::int64_t& year, unsigned& month, unsigned& day) noexcept {
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
  const unsigned year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
  const std::int64_t year_base = static_cast<std::int64_t>(year_of_era) + era * 400;
  const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const unsigned month_prime = (5U * day_of_year + 2U) / 153U;
  day = day_of_year - (153U * month_prime + 2U) / 5U + 1U;
  month = static_cast<unsigned>(static_cast<int>(month_prime) + (month_prime < 10U ? 3 : -9));
  year = year_base + (month <= 2U ? 1 : 0);
}

void append_padded(std::string& out, std::int64_t value, std::size_t width) {
  const bool negative = value < 0;
  std::string digits = std::to_string(magnitude(value));
  if (digits.size() < width) {
    digits.insert(0, width - digits.size(), '0');
  }
  if (negative) {
    out.push_back('-');
  }
  out.append(digits);
}

bool is_digit(char c) noexcept { return c >= '0' && c <= '9'; }

Outcome<std::int64_t> parse_fixed_digits(std::string_view text, std::size_t offset, std::size_t count) {
  if (offset + count > text.size()) {
    return Outcome<std::int64_t>::fail(ReasonCode::invalid_argument, "timestamp literal is truncated");
  }
  std::int64_t value = 0;
  for (std::size_t index = 0; index < count; ++index) {
    const char c = text[offset + index];
    if (!is_digit(c)) {
      return Outcome<std::int64_t>::fail(ReasonCode::invalid_argument,
                                         "timestamp literal has a non-digit where a digit is required");
    }
    std::int64_t shifted = 0;
    if (!checked_mul(value, 10, shifted) || !checked_add(shifted, static_cast<std::int64_t>(c - '0'), value)) {
      return Outcome<std::int64_t>::fail(ReasonCode::arithmetic_overflow, "timestamp field overflow");
    }
  }
  return Outcome<std::int64_t>::ok(value);
}

}  // namespace

Outcome<Timestamp> Timestamp::from_unix_nanos(UnixNanos nanos) {
  if (nanos == kUnsetValue) {
    return Outcome<Timestamp>::fail(ReasonCode::value_out_of_range, "unset is not a representable instant");
  }
  return Outcome<Timestamp>::ok(Timestamp(nanos));
}

std::string Timestamp::to_rfc3339() const {
  if (!is_set()) {
    return std::string("unset");
  }
  std::int64_t seconds = nanos_ / kNanosPerSecond;
  std::int64_t fraction = nanos_ % kNanosPerSecond;
  if (fraction < 0) {
    fraction += kNanosPerSecond;
    --seconds;
  }
  std::int64_t days = seconds / kSecondsPerDay;
  std::int64_t second_of_day = seconds % kSecondsPerDay;
  if (second_of_day < 0) {
    second_of_day += kSecondsPerDay;
    --days;
  }
  std::int64_t year = 0;
  unsigned month = 0;
  unsigned day = 0;
  civil_from_days(days, year, month, day);

  std::string out;
  out.reserve(32);
  append_padded(out, year, 4);
  out.push_back('-');
  append_padded(out, static_cast<std::int64_t>(month), 2);
  out.push_back('-');
  append_padded(out, static_cast<std::int64_t>(day), 2);
  out.push_back('T');
  append_padded(out, second_of_day / 3600, 2);
  out.push_back(':');
  append_padded(out, (second_of_day % 3600) / 60, 2);
  out.push_back(':');
  append_padded(out, second_of_day % 60, 2);
  if (fraction != 0) {
    std::size_t width = 9;
    if (fraction % 1000000LL == 0) {
      width = 3;
    } else if (fraction % 1000LL == 0) {
      width = 6;
    }
    std::string digits = std::to_string(fraction);
    digits.insert(0, 9 - digits.size(), '0');
    digits.resize(width);
    out.push_back('.');
    out.append(digits);
  }
  out.push_back('Z');
  return out;
}

Outcome<Timestamp> Timestamp::parse_rfc3339(std::string_view text) {
  // "1970-01-01T00:00:00Z" is the shortest accepted form.
  if (text.size() < 20) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp literal is too short");
  }
  auto year = parse_fixed_digits(text, 0, 4);
  if (!year) {
    return year.propagate<Timestamp>();
  }
  if (text[4] != '-' || text[7] != '-') {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp requires '-' date separators");
  }
  auto month = parse_fixed_digits(text, 5, 2);
  if (!month) {
    return month.propagate<Timestamp>();
  }
  auto day = parse_fixed_digits(text, 8, 2);
  if (!day) {
    return day.propagate<Timestamp>();
  }
  if (text[10] != 'T') {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp requires an upper-case 'T' separator");
  }
  if (text[13] != ':' || text[16] != ':') {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp requires ':' time separators");
  }
  auto hour = parse_fixed_digits(text, 11, 2);
  if (!hour) {
    return hour.propagate<Timestamp>();
  }
  auto minute = parse_fixed_digits(text, 14, 2);
  if (!minute) {
    return minute.propagate<Timestamp>();
  }
  auto second = parse_fixed_digits(text, 17, 2);
  if (!second) {
    return second.propagate<Timestamp>();
  }
  if (month.value() < 1 || month.value() > 12) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp month out of range");
  }
  if (day.value() < 1 || day.value() > 31) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp day out of range");
  }
  if (hour.value() > 23 || minute.value() > 59) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp time of day out of range");
  }
  if (second.value() > 59) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument,
                                    "leap seconds are not representable and are rejected");
  }

  std::size_t cursor = 19;
  std::int64_t fraction_nanos = 0;
  if (cursor < text.size() && text[cursor] == '.') {
    ++cursor;
    const std::size_t fraction_start = cursor;
    while (cursor < text.size() && is_digit(text[cursor])) {
      ++cursor;
    }
    const std::size_t digits = cursor - fraction_start;
    if (digits == 0 || digits > 9) {
      return Outcome<Timestamp>::fail(ReasonCode::invalid_argument,
                                      "timestamp fraction must contain between 1 and 9 digits");
    }
    auto parsed = parse_fixed_digits(text, fraction_start, digits);
    if (!parsed) {
      return parsed.propagate<Timestamp>();
    }
    fraction_nanos = parsed.value();
    for (std::size_t padding = digits; padding < 9; ++padding) {
      if (!checked_mul(fraction_nanos, 10, fraction_nanos)) {
        return Outcome<Timestamp>::fail(ReasonCode::arithmetic_overflow, "timestamp fraction overflow");
      }
    }
  }

  std::int64_t offset_seconds = 0;
  if (cursor >= text.size()) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp is missing its zone designator");
  }
  if (text[cursor] == 'Z') {
    ++cursor;
  } else if (text[cursor] == '+' || text[cursor] == '-') {
    const bool negative_offset = text[cursor] == '-';
    if (cursor + 6 > text.size() || text[cursor + 3] != ':') {
      return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp offset must be (+|-)HH:MM");
    }
    auto offset_hours = parse_fixed_digits(text, cursor + 1, 2);
    if (!offset_hours) {
      return offset_hours.propagate<Timestamp>();
    }
    auto offset_minutes = parse_fixed_digits(text, cursor + 4, 2);
    if (!offset_minutes) {
      return offset_minutes.propagate<Timestamp>();
    }
    if (offset_hours.value() > 23 || offset_minutes.value() > 59) {
      return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp offset out of range");
    }
    offset_seconds = offset_hours.value() * 3600 + offset_minutes.value() * 60;
    if (negative_offset) {
      offset_seconds = -offset_seconds;
    }
    cursor += 6;
  } else {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp zone designator must be 'Z' or an offset");
  }
  if (cursor != text.size()) {
    return Outcome<Timestamp>::fail(ReasonCode::invalid_argument, "timestamp has trailing characters");
  }

  const std::int64_t days = days_from_civil(year.value(), static_cast<unsigned>(month.value()),
                                            static_cast<unsigned>(day.value()));
  const std::int64_t second_of_day = hour.value() * 3600 + minute.value() * 60 + second.value();
  std::int64_t total_seconds = 0;
  if (!checked_mul(days, kSecondsPerDay, total_seconds) ||
      !checked_add(total_seconds, second_of_day, total_seconds) ||
      !checked_sub(total_seconds, offset_seconds, total_seconds)) {
    return Outcome<Timestamp>::fail(ReasonCode::arithmetic_overflow, "timestamp out of representable range");
  }
  std::int64_t nanos = 0;
  if (!checked_mul(total_seconds, kNanosPerSecond, nanos) ||
      !checked_add(nanos, fraction_nanos, nanos)) {
    return Outcome<Timestamp>::fail(ReasonCode::arithmetic_overflow, "timestamp out of representable range");
  }
  return from_unix_nanos(nanos);
}

Duration saturating_add(Duration lhs, Duration rhs) noexcept {
  std::int64_t sum = 0;
  if (!checked_add(lhs.count(), rhs.count(), sum)) {
    return rhs.count() > 0 ? Duration::max() : Duration::min();
  }
  return Duration(sum);
}

Duration saturating_sub(Duration lhs, Duration rhs) noexcept {
  std::int64_t difference = 0;
  if (!checked_sub(lhs.count(), rhs.count(), difference)) {
    return rhs.count() > 0 ? Duration::min() : Duration::max();
  }
  return Duration(difference);
}

Outcome<Duration> duration_between(Timestamp later, Timestamp earlier) {
  if (!later.is_set() || !earlier.is_set()) {
    return Outcome<Duration>::fail(ReasonCode::no_evidence, "elapsed time requires two set instants");
  }
  std::int64_t difference = 0;
  if (!checked_sub(later.unix_nanos(), earlier.unix_nanos(), difference)) {
    return Outcome<Duration>::fail(ReasonCode::arithmetic_overflow, "elapsed time out of representable range");
  }
  return Outcome<Duration>::ok(Duration(difference));
}

Outcome<Duration> parse_duration(std::string_view text) {
  if (text.empty()) {
    return Outcome<Duration>::fail(ReasonCode::empty_input, "empty duration literal");
  }
  std::size_t cursor = 0;
  std::int64_t total = 0;
  bool parsed_any = false;
  while (cursor < text.size()) {
    const std::size_t number_start = cursor;
    while (cursor < text.size() && is_digit(text[cursor])) {
      ++cursor;
    }
    if (cursor == number_start) {
      return Outcome<Duration>::fail(ReasonCode::invalid_argument, "duration literal must start a term with a digit");
    }
    auto amount = parse_fixed_digits(text, number_start, cursor - number_start);
    if (!amount) {
      return amount.propagate<Duration>();
    }
    std::int64_t multiplier = 0;
    if (text.compare(cursor, 2, "ms") == 0) {
      multiplier = 1000000LL;
      cursor += 2;
    } else if (text.compare(cursor, 2, "us") == 0) {
      multiplier = 1000LL;
      cursor += 2;
    } else if (text.compare(cursor, 2, "ns") == 0) {
      multiplier = 1LL;
      cursor += 2;
    } else if (cursor < text.size() && text[cursor] == 'd') {
      multiplier = kNanosPerDay;
      cursor += 1;
    } else if (cursor < text.size() && text[cursor] == 'h') {
      multiplier = kNanosPerHour;
      cursor += 1;
    } else if (cursor < text.size() && text[cursor] == 'm') {
      multiplier = kNanosPerMinute;
      cursor += 1;
    } else if (cursor < text.size() && text[cursor] == 's') {
      multiplier = kNanosPerSecond;
      cursor += 1;
    } else {
      return Outcome<Duration>::fail(ReasonCode::invalid_argument, "duration literal has an unknown unit suffix");
    }
    std::int64_t term = 0;
    if (!checked_mul(amount.value(), multiplier, term) || !checked_add(total, term, total)) {
      return Outcome<Duration>::fail(ReasonCode::arithmetic_overflow, "duration literal overflows");
    }
    parsed_any = true;
  }
  if (!parsed_any) {
    return Outcome<Duration>::fail(ReasonCode::empty_input, "duration literal has no terms");
  }
  return Outcome<Duration>::ok(Duration(total));
}

std::string to_string(Duration duration) {
  const std::int64_t count = duration.count();
  if (count == 0) {
    return std::string("0ms");
  }
  const bool negative = count < 0;
  const auto magnitude_count = static_cast<std::int64_t>(magnitude(count));
  std::string result;
  if (negative) {
    result.push_back('-');
  }
  const std::int64_t units[6] = {kNanosPerDay, kNanosPerHour, kNanosPerMinute, kNanosPerSecond, 1000000LL, 1000LL};
  const char* suffixes[6] = {"d", "h", "m", "s", "ms", "us"};
  for (std::size_t index = 0; index < 6; ++index) {
    if (magnitude_count % units[index] == 0) {
      result.append(std::to_string(magnitude_count / units[index]));
      result.append(suffixes[index]);
      return result;
    }
  }
  result.append(std::to_string(magnitude_count));
  result.append("ns");
  return result;
}

Timestamp SystemClock::now() const {
  const auto since_epoch = std::chrono::system_clock::now().time_since_epoch();
  const auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(since_epoch).count();
  auto stamp = Timestamp::from_unix_nanos(static_cast<UnixNanos>(nanos));
  if (!stamp) {
    return Timestamp{};
  }
  return stamp.value();
}

ManualClock::ManualClock(Timestamp start) : now_(start.is_set() ? start : Timestamp::from_unix_nanos(0).value()) {}

Timestamp ManualClock::now() const {
  const std::lock_guard<std::mutex> guard(mutex_);
  return now_;
}

void ManualClock::set(Timestamp value) noexcept {
  const std::lock_guard<std::mutex> guard(mutex_);
  if (value.is_set()) {
    now_ = value;
  }
}

Outcome<Duration> ManualClock::advance(Duration delta) noexcept {
  if (delta.count() < 0) {
    return Outcome<Duration>::fail(ReasonCode::invalid_argument, "a manual clock cannot move backwards");
  }
  const std::lock_guard<std::mutex> guard(mutex_);
  std::int64_t updated = 0;
  if (!checked_add(now_.unix_nanos(), delta.count(), updated)) {
    return Outcome<Duration>::fail(ReasonCode::arithmetic_overflow, "manual clock overflowed");
  }
  auto stamp = Timestamp::from_unix_nanos(updated);
  if (!stamp) {
    return Outcome<Duration>::fail(ReasonCode::arithmetic_overflow, "manual clock left the representable range");
  }
  now_ = stamp.value();
  return Outcome<Duration>::ok(delta);
}

}  // namespace fo
