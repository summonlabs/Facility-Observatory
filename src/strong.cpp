// Facility Observatory - strong value types.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/strong.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "facility_observatory/checked.hpp"

namespace fo {
namespace {

constexpr std::array<std::int64_t, 10> kPow10{{1LL,
                                               10LL,
                                               100LL,
                                               1000LL,
                                               10000LL,
                                               100000LL,
                                               1000000LL,
                                               10000000LL,
                                               100000000LL,
                                               1000000000LL}};

constexpr bool is_ascii_alnum(char c) noexcept {
  return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

constexpr bool is_token_char(char c) noexcept {
  return is_ascii_alnum(c) || c == '.' || c == '_' || c == ':' || c == '-';
}

constexpr bool is_aspect_body_char(char c) noexcept {
  return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
}

// Rebuilds a signed value from a sign plus unsigned magnitude, rejecting
// anything that does not fit in int64.
Outcome<FixedPoint> from_sign_magnitude(bool negative, std::uint64_t mag, int scale) {
  constexpr std::uint64_t kInt64MinMagnitude = static_cast<std::uint64_t>(1) << 63;
  if (negative) {
    if (mag > kInt64MinMagnitude) {
      return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                       "fixed point quotient below representable minimum");
    }
    if (mag == kInt64MinMagnitude) {
      return FixedPoint::from_scaled(std::numeric_limits<std::int64_t>::min(), scale);
    }
    return FixedPoint::from_scaled(-static_cast<std::int64_t>(mag), scale);
  }
  if (mag > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                     "fixed point quotient above representable maximum");
  }
  return FixedPoint::from_scaled(static_cast<std::int64_t>(mag), scale);
}

constexpr std::array<UnitInfo, 21> kUnitTable{{
    {Unit::none, "none", Unit::none, 1, 1, 0, 0},
    {Unit::count, "count", Unit::count, 1, 1, 0, 0},
    {Unit::ratio, "ratio", Unit::ratio, 1, 1, 0, 0},
    {Unit::percent, "percent", Unit::ratio, 1, 100, 0, 0},
    {Unit::parts_per_million, "ppm", Unit::ratio, 1, 1000000, 0, 0},
    {Unit::bytes, "bytes", Unit::bytes, 1, 1, 0, 0},
    {Unit::seconds, "seconds", Unit::seconds, 1, 1, 0, 0},
    {Unit::watts, "watts", Unit::watts, 1, 1, 0, 0},
    {Unit::kilowatts, "kilowatts", Unit::watts, 1000, 1, 0, 0},
    {Unit::megawatts, "megawatts", Unit::watts, 1000000, 1, 0, 0},
    {Unit::kilowatt_hours, "kilowatt_hours", Unit::kilowatt_hours, 1, 1, 0, 0},
    {Unit::joules, "joules", Unit::joules, 1, 1, 0, 0},
    {Unit::celsius, "celsius", Unit::kelvin, 1, 1, 27315, 2},
    {Unit::kelvin, "kelvin", Unit::kelvin, 1, 1, 0, 0},
    {Unit::volts, "volts", Unit::volts, 1, 1, 0, 0},
    {Unit::amperes, "amperes", Unit::amperes, 1, 1, 0, 0},
    {Unit::hertz, "hertz", Unit::hertz, 1, 1, 0, 0},
    {Unit::pascals, "pascals", Unit::pascals, 1, 1, 0, 0},
    {Unit::liters_per_second, "liters_per_second", Unit::liters_per_second, 1, 1, 0, 0},
    {Unit::cubic_meters_per_second, "cubic_meters_per_second", Unit::liters_per_second, 1000, 1, 0, 0},
    {Unit::usd, "usd", Unit::usd, 1, 1, 0, 0},
}};

}  // namespace

// ---------------------------------------------------------------------------
// Name validation
// ---------------------------------------------------------------------------
std::string_view to_string(NameClass name_class) noexcept {
  switch (name_class) {
    case NameClass::authority:
      return std::string_view{"authority"};
    case NameClass::source:
      return std::string_view{"source"};
    case NameClass::entity:
      return std::string_view{"entity"};
    case NameClass::aspect:
      return std::string_view{"aspect"};
    case NameClass::token:
      return std::string_view{"token"};
  }
  return std::string_view{"unknown"};
}

bool is_valid_utf8(std::string_view text) noexcept {
  std::size_t index = 0;
  const std::size_t size = text.size();
  while (index < size) {
    const auto lead = static_cast<unsigned char>(text[index]);
    if (lead == 0x00U) {
      return false;
    }
    if (lead < 0x80U) {
      ++index;
      continue;
    }
    std::size_t continuation = 0;
    std::uint32_t code_point = 0;
    if ((lead & 0xE0U) == 0xC0U) {
      continuation = 1;
      code_point = static_cast<std::uint32_t>(lead & 0x1FU);
    } else if ((lead & 0xF0U) == 0xE0U) {
      continuation = 2;
      code_point = static_cast<std::uint32_t>(lead & 0x0FU);
    } else if ((lead & 0xF8U) == 0xF0U) {
      continuation = 3;
      code_point = static_cast<std::uint32_t>(lead & 0x07U);
    } else {
      return false;  // stray continuation byte or 5/6-byte form
    }
    if (index + continuation >= size) {
      return false;  // truncated sequence
    }
    for (std::size_t offset = 1; offset <= continuation; ++offset) {
      const auto trail = static_cast<unsigned char>(text[index + offset]);
      if ((trail & 0xC0U) != 0x80U) {
        return false;
      }
      code_point = (code_point << 6) | static_cast<std::uint32_t>(trail & 0x3FU);
    }
    if ((continuation == 1 && code_point < 0x80U) || (continuation == 2 && code_point < 0x800U) ||
        (continuation == 3 && code_point < 0x10000U)) {
      return false;  // overlong encoding
    }
    if (code_point > 0x10FFFFU) {
      return false;
    }
    if (code_point >= 0xD800U && code_point <= 0xDFFFU) {
      return false;  // UTF-16 surrogate
    }
    index += continuation + 1;
  }
  return true;
}

Status validate_name(std::string_view text, NameClass name_class, std::size_t max_length) {
  if (text.empty()) {
    return Status::fail(ReasonCode::empty_input, "identifier is empty");
  }
  if (text.size() > max_length) {
    return Status::fail(ReasonCode::identifier_too_long,
                        "identifier of " + std::to_string(text.size()) + " bytes exceeds bound of " +
                            std::to_string(max_length) + " bytes");
  }
  switch (name_class) {
    case NameClass::authority:
    case NameClass::source:
    case NameClass::token: {
      if (!is_ascii_alnum(text.front())) {
        return Status::fail(ReasonCode::invalid_identifier,
                            "identifier must begin with an ASCII alphanumeric character");
      }
      for (const char c : text) {
        if (!is_token_char(c)) {
          return Status::fail(ReasonCode::invalid_identifier,
                              std::string("identifier contains a character outside [A-Za-z0-9._:-] at offset ") +
                                  std::to_string(static_cast<std::size_t>(&c - text.data())));
        }
      }
      return success();
    }
    case NameClass::aspect: {
      if (!(text.front() >= 'a' && text.front() <= 'z')) {
        return Status::fail(ReasonCode::invalid_aspect_path, "aspect must begin with a lower-case letter");
      }
      bool previous_was_dot = false;
      for (std::size_t index = 0; index < text.size(); ++index) {
        const char c = text[index];
        if (c == '.') {
          if (previous_was_dot) {
            return Status::fail(ReasonCode::invalid_aspect_path,
                                "aspect contains an empty segment at offset " + std::to_string(index));
          }
          previous_was_dot = true;
          continue;
        }
        previous_was_dot = false;
        if (!is_aspect_body_char(c)) {
          return Status::fail(ReasonCode::invalid_aspect_path,
                              "aspect contains an invalid character at offset " + std::to_string(index));
        }
      }
      if (previous_was_dot) {
        return Status::fail(ReasonCode::invalid_aspect_path, "aspect has a trailing separator");
      }
      return success();
    }
    case NameClass::entity: {
      if (!is_valid_utf8(text)) {
        return Status::fail(ReasonCode::malformed_encoding, "entity identifier is not well-formed UTF-8");
      }
      for (std::size_t index = 0; index < text.size(); ++index) {
        const auto c = static_cast<unsigned char>(text[index]);
        if (c < 0x20U || c == 0x7FU) {
          return Status::fail(ReasonCode::invalid_identifier,
                              "entity identifier contains a control character at offset " + std::to_string(index));
        }
      }
      return success();
    }
  }
  return Status::fail(ReasonCode::invalid_argument, "unknown name class");
}

Outcome<Name> Name::parse(std::string_view text, NameClass name_class, std::size_t max_length) {
  auto validated = validate_name(text, name_class, max_length);
  if (!validated) {
    return validated.propagate<Name>();
  }
  return Outcome<Name>::ok(Name(std::string(text)));
}

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------
const UnitInfo& unit_info(Unit unit) noexcept {
  for (const UnitInfo& info : kUnitTable) {
    if (info.unit == unit) {
      return info;
    }
  }
  return kUnitTable[0];
}

bool is_known_unit(Unit unit) noexcept {
  for (const UnitInfo& info : kUnitTable) {
    if (info.unit == unit) {
      return true;
    }
  }
  return false;
}

std::string_view to_string(Unit unit) noexcept { return unit_info(unit).name; }

Outcome<Unit> unit_from_string(std::string_view text) {
  for (const UnitInfo& info : kUnitTable) {
    if (info.name == text) {
      return Outcome<Unit>::ok(info.unit);
    }
  }
  return Outcome<Unit>::fail(ReasonCode::unit_incompatible, "unknown unit '" + std::string(text) + "'");
}

Unit canonical_unit(Unit unit) noexcept { return unit_info(unit).canonical; }

bool units_compatible(Unit lhs, Unit rhs) noexcept {
  if (!is_known_unit(lhs) || !is_known_unit(rhs)) {
    return false;
  }
  return canonical_unit(lhs) == canonical_unit(rhs);
}

std::string_view to_string(RoundingMode mode) noexcept {
  switch (mode) {
    case RoundingMode::reject_inexact:
      return std::string_view{"reject_inexact"};
    case RoundingMode::half_even:
      return std::string_view{"half_even"};
    case RoundingMode::toward_zero:
      return std::string_view{"toward_zero"};
    case RoundingMode::floor:
      return std::string_view{"floor"};
  }
  return std::string_view{"unknown"};
}

Outcome<std::int64_t> pow10(int exponent) {
  if (exponent < 0 || exponent > FixedPoint::kMaxScale) {
    return Outcome<std::int64_t>::fail(ReasonCode::value_out_of_range,
                                       "power of ten exponent out of range: " + std::to_string(exponent));
  }
  return Outcome<std::int64_t>::ok(kPow10[static_cast<std::size_t>(exponent)]);
}

// ---------------------------------------------------------------------------
// FixedPoint
// ---------------------------------------------------------------------------
Outcome<FixedPoint> FixedPoint::from_scaled(std::int64_t mantissa, int scale) {
  if (scale < kMinScale || scale > kMaxScale) {
    return Outcome<FixedPoint>::fail(ReasonCode::scale_mismatch,
                                     "scale " + std::to_string(scale) + " outside [0, 9]");
  }
  return Outcome<FixedPoint>::ok(FixedPoint(mantissa, scale));
}

Outcome<FixedPoint> FixedPoint::parse(std::string_view text) {
  if (text.empty()) {
    return Outcome<FixedPoint>::fail(ReasonCode::empty_input, "empty fixed point literal");
  }
  std::size_t index = 0;
  const bool negative = text.front() == '-';
  if (text.front() == '-' || text.front() == '+') {
    index = 1;
  }
  if (index >= text.size() || !(text[index] >= '0' && text[index] <= '9')) {
    return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument,
                                     "fixed point literal must have a digit before any separator");
  }
  // Exactly one spelling is accepted for each value, so parsing and rendering are
  // a bijection on canonical forms: no leading zeros, and no negative zero.
  if (text[index] == '0' && index + 1 < text.size() && text[index + 1] >= '0' && text[index + 1] <= '9') {
    return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument,
                                     "fixed point literal has leading zeros");
  }
  std::int64_t mantissa = 0;
  int scale = 0;
  bool seen_separator = false;
  for (; index < text.size(); ++index) {
    const char c = text[index];
    if (c == '.') {
      if (seen_separator) {
        return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument, "fixed point literal has two separators");
      }
      seen_separator = true;
      continue;
    }
    if (c < '0' || c > '9') {
      return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument,
                                       std::string("fixed point literal contains '") + c + "' at offset " +
                                           std::to_string(index));
    }
    if (seen_separator) {
      if (scale == kMaxScale) {
        return Outcome<FixedPoint>::fail(ReasonCode::scale_mismatch,
                                         "fixed point literal has more than 9 fractional digits");
      }
      ++scale;
    }
    std::int64_t shifted = 0;
    if (!checked_mul(mantissa, 10, shifted)) {
      return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "fixed point literal overflows int64");
    }
    std::int64_t accumulated = 0;
    if (!checked_add(shifted, static_cast<std::int64_t>(c - '0'), accumulated)) {
      return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "fixed point literal overflows int64");
    }
    mantissa = accumulated;
  }
  if (seen_separator && scale == 0) {
    return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument,
                                     "fixed point literal has a separator with no fractional digits");
  }
  if (negative) {
    if (mantissa == 0) {
      return Outcome<FixedPoint>::fail(ReasonCode::invalid_argument,
                                       "negative zero is not a canonical fixed point literal");
    }
    if (mantissa == std::numeric_limits<std::int64_t>::min()) {
      return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "fixed point literal overflows int64");
    }
    mantissa = -mantissa;
  }
  return from_scaled(mantissa, scale);
}

std::string FixedPoint::to_string() const {
  const bool negative = mantissa_ < 0;
  std::string digits = std::to_string(magnitude(mantissa_));
  if (scale_ > 0) {
    const auto scale = static_cast<std::size_t>(scale_);
    if (digits.size() <= scale) {
      digits.insert(0, scale + 1 - digits.size(), '0');
    }
    digits.insert(digits.size() - scale, 1, '.');
  }
  if (negative) {
    digits.insert(0, 1, '-');
  }
  return digits;
}

FixedPoint FixedPoint::reduced() const noexcept {
  std::int64_t mantissa = mantissa_;
  int scale = scale_;
  while (scale > 0 && mantissa % 10 == 0) {
    mantissa /= 10;
    --scale;
  }
  return FixedPoint(mantissa, scale);
}

Outcome<std::strong_ordering> FixedPoint::compare(const FixedPoint& other) const {
  const int target = std::max(scale_, other.scale_);
  auto left = rescale(target);
  if (!left) {
    return left.propagate<std::strong_ordering>();
  }
  auto right = other.rescale(target);
  if (!right) {
    return right.propagate<std::strong_ordering>();
  }
  const std::int64_t a = left.value().mantissa();
  const std::int64_t b = right.value().mantissa();
  if (a < b) {
    return Outcome<std::strong_ordering>::ok(std::strong_ordering::less);
  }
  if (a > b) {
    return Outcome<std::strong_ordering>::ok(std::strong_ordering::greater);
  }
  return Outcome<std::strong_ordering>::ok(std::strong_ordering::equal);
}

Outcome<FixedPoint> FixedPoint::rescale(int new_scale, RoundingMode mode) const {
  if (new_scale < kMinScale || new_scale > kMaxScale) {
    return Outcome<FixedPoint>::fail(ReasonCode::scale_mismatch,
                                     "target scale " + std::to_string(new_scale) + " outside [0, 9]");
  }
  if (new_scale == scale_) {
    return Outcome<FixedPoint>::ok(*this);
  }
  if (new_scale > scale_) {
    std::int64_t grown = 0;
    if (!checked_mul(mantissa_, kPow10[static_cast<std::size_t>(new_scale - scale_)], grown)) {
      return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                       "rescaling " + to_string() + " to scale " + std::to_string(new_scale) +
                                           " overflows int64");
    }
    return from_scaled(grown, new_scale);
  }
  return scale_divide(mantissa_, scale_, kPow10[static_cast<std::size_t>(scale_ - new_scale)], mode);
}

Outcome<FixedPoint> FixedPoint::scale_divide(std::int64_t mantissa, int scale, std::int64_t divisor,
                                             RoundingMode mode) {
  if (divisor == 0) {
    return Outcome<FixedPoint>::fail(ReasonCode::division_by_zero, "fixed point division by zero");
  }
  // The divisor magnitude is computed in unsigned arithmetic, so INT64_MIN is
  // handled by the general path rather than by a special case.
  const bool negative = (mantissa < 0) != (divisor < 0);
  std::uint64_t mag = magnitude(mantissa);
  const std::uint64_t div = magnitude(divisor);

  if (mag == 0) {
    return from_scaled(0, scale);
  }

  const int maximum_growth = kMaxScale - scale;
  std::uint64_t best_quotient = 0;
  std::uint64_t best_remainder = 0;
  int best_growth = 0;
  bool have_candidate = false;
  for (int growth = 0; growth <= maximum_growth; ++growth) {
    std::uint64_t scaled = 0;
    if (!checked_mul_u64(mag, static_cast<std::uint64_t>(kPow10[static_cast<std::size_t>(growth)]), scaled)) {
      break;  // further growth cannot help and would overflow the working value
    }
    const std::uint64_t quotient = scaled / div;
    const std::uint64_t remainder = scaled % div;
    best_quotient = quotient;
    best_remainder = remainder;
    best_growth = growth;
    have_candidate = true;
    if (remainder == 0) {
      return from_sign_magnitude(negative, quotient, scale + growth);
    }
  }
  if (!have_candidate) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "fixed point division overflowed its working range");
  }
  if (mode == RoundingMode::reject_inexact) {
    return Outcome<FixedPoint>::fail(ReasonCode::inexact_conversion,
                                     "division is not exact at the maximum representable scale");
  }
  const int result_scale = scale + best_growth;
  std::uint64_t rounded = best_quotient;
  switch (mode) {
    case RoundingMode::half_even: {
      const std::uint64_t twice = best_remainder * 2;
      if (twice > div || (twice == div && (best_quotient % 2) != 0)) {
        ++rounded;
      }
      break;
    }
    case RoundingMode::toward_zero:
      break;
    case RoundingMode::floor: {
      if (negative && best_remainder != 0) {
        ++rounded;
      }
      break;
    }
    case RoundingMode::reject_inexact:
      break;
  }
  return from_sign_magnitude(negative, rounded, result_scale);
}

Outcome<FixedPoint> FixedPoint::add(const FixedPoint& other) const {
  const int target = std::max(scale_, other.scale_);
  auto left = rescale(target);
  if (!left) {
    return left.propagate<FixedPoint>();
  }
  auto right = other.rescale(target);
  if (!right) {
    return right.propagate<FixedPoint>();
  }
  std::int64_t sum = 0;
  if (!checked_add(left.value().mantissa(), right.value().mantissa(), sum)) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                     "sum of " + to_string() + " and " + other.to_string() + " overflows int64");
  }
  return from_scaled(sum, target);
}

Outcome<FixedPoint> FixedPoint::subtract(const FixedPoint& other) const {
  const int target = std::max(scale_, other.scale_);
  auto left = rescale(target);
  if (!left) {
    return left.propagate<FixedPoint>();
  }
  auto right = other.rescale(target);
  if (!right) {
    return right.propagate<FixedPoint>();
  }
  std::int64_t difference = 0;
  if (!checked_sub(left.value().mantissa(), right.value().mantissa(), difference)) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                     "difference of " + to_string() + " and " + other.to_string() +
                                         " overflows int64");
  }
  return from_scaled(difference, target);
}

Outcome<FixedPoint> FixedPoint::multiply(std::int64_t factor) const {
  std::int64_t product = 0;
  if (!checked_mul(mantissa_, factor, product)) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                     "product of " + to_string() + " and " + std::to_string(factor) +
                                         " overflows int64");
  }
  return from_scaled(product, scale_);
}

Outcome<FixedPoint> FixedPoint::multiply(const FixedPoint& other) const {
  const int target = scale_ + other.scale_;
  if (target > kMaxScale) {
    return Outcome<FixedPoint>::fail(ReasonCode::scale_mismatch,
                                     "product scale " + std::to_string(target) + " exceeds 9");
  }
  std::int64_t product = 0;
  if (!checked_mul(mantissa_, other.mantissa(), product)) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow,
                                     "product of " + to_string() + " and " + other.to_string() +
                                         " overflows int64");
  }
  return from_scaled(product, target);
}

Outcome<FixedPoint> FixedPoint::divide(std::int64_t divisor, RoundingMode mode) const {
  return scale_divide(mantissa_, scale_, divisor, mode);
}

Outcome<FixedPoint> FixedPoint::negated() const {
  if (mantissa_ == std::numeric_limits<std::int64_t>::min()) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "negation of the minimum int64 value");
  }
  return from_scaled(-mantissa_, scale_);
}

Outcome<FixedPoint> FixedPoint::absolute() const {
  if (mantissa_ == std::numeric_limits<std::int64_t>::min()) {
    return Outcome<FixedPoint>::fail(ReasonCode::arithmetic_overflow, "absolute value of the minimum int64 value");
  }
  return from_scaled(mantissa_ < 0 ? -mantissa_ : mantissa_, scale_);
}

// ---------------------------------------------------------------------------
// Unit conversion
// ---------------------------------------------------------------------------
Outcome<FixedPoint> to_canonical(const FixedPoint& value, Unit from) {
  if (!is_known_unit(from)) {
    return Outcome<FixedPoint>::fail(ReasonCode::unit_incompatible, "unknown source unit");
  }
  const UnitInfo& info = unit_info(from);
  auto scaled = value.multiply(info.numer);
  if (!scaled) {
    return scaled.propagate<FixedPoint>();
  }
  auto divided = scaled.value().divide(info.denom, RoundingMode::reject_inexact);
  if (!divided) {
    if (divided.code() == ReasonCode::inexact_conversion) {
      return Outcome<FixedPoint>::fail(ReasonCode::inexact_conversion,
                                       "conversion from " + std::string(info.name) + " is not exact for value " +
                                           value.to_string());
    }
    return divided.propagate<FixedPoint>();
  }
  if (info.offset_mantissa == 0) {
    return divided;
  }
  auto offset = FixedPoint::from_scaled(info.offset_mantissa, info.offset_scale);
  if (!offset) {
    return offset.propagate<FixedPoint>();
  }
  auto shifted = divided.value().add(offset.value());
  if (!shifted) {
    return shifted.propagate<FixedPoint>();
  }
  return shifted;
}

Outcome<FixedPoint> convert(const FixedPoint& value, Unit from, Unit to) {
  if (from == to) {
    return Outcome<FixedPoint>::ok(value);
  }
  if (!units_compatible(from, to)) {
    return Outcome<FixedPoint>::fail(ReasonCode::unit_incompatible,
                                     "cannot convert " + std::string(to_string(from)) + " to " +
                                         std::string(to_string(to)));
  }
  auto canonical = to_canonical(value, from);
  if (!canonical) {
    return canonical.propagate<FixedPoint>();
  }
  const UnitInfo& target = unit_info(to);
  Outcome<FixedPoint> current = canonical;
  if (target.offset_mantissa != 0) {
    auto offset = FixedPoint::from_scaled(target.offset_mantissa, target.offset_scale);
    if (!offset) {
      return offset.propagate<FixedPoint>();
    }
    auto shifted = current.value().subtract(offset.value());
    if (!shifted) {
      return shifted.propagate<FixedPoint>();
    }
    current = shifted;
  }
  auto rescaled = current.value().multiply(target.denom);
  if (!rescaled) {
    return rescaled.propagate<FixedPoint>();
  }
  auto finished = rescaled.value().divide(target.numer, RoundingMode::reject_inexact);
  if (!finished) {
    if (finished.code() == ReasonCode::inexact_conversion) {
      return Outcome<FixedPoint>::fail(ReasonCode::inexact_conversion,
                                       "conversion to " + std::string(target.name) + " is not exact for value " +
                                           value.to_string());
    }
    return finished.propagate<FixedPoint>();
  }
  return finished;
}

}  // namespace fo
