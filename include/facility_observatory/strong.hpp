// Facility Observatory - strong value types.
//
// Identities, generations, epochs, revisions and quantities are distinct types.
// Interchanging them is a compile error, not a code review question.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_STRONG_HPP
#define FACILITY_OBSERVATORY_STRONG_HPP

#include <compare>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"

namespace fo {

// ---------------------------------------------------------------------------
// Text validation
// ---------------------------------------------------------------------------
enum class NameClass : std::uint8_t {
  // [A-Za-z0-9][A-Za-z0-9._:-]*
  authority = 0,
  // [A-Za-z0-9][A-Za-z0-9._:-]*
  source = 1,
  // 1..max bytes of printable text; UTF-8 sequences must be well formed and no
  // control character (including DEL) is permitted.
  entity = 2,
  // [a-z][a-z0-9_]*(\.[a-z0-9_]+)*
  aspect = 3,
  // [A-Za-z0-9][A-Za-z0-9._:-]*
  token = 4,
};

FO_API std::string_view to_string(NameClass name_class) noexcept;

// Strict UTF-8 validation: rejects overlong encodings, UTF-16 surrogate code
// points, and code points above U+10FFFF. ASCII NUL is rejected.
FO_API bool is_valid_utf8(std::string_view text) noexcept;

// Validates text against a class and a hard length bound.
FO_API Status validate_name(std::string_view text, NameClass name_class, std::size_t max_length);

// ---------------------------------------------------------------------------
// Name
// ---------------------------------------------------------------------------
class FO_API Name {
 public:
  Name() = default;

  static Outcome<Name> parse(std::string_view text, NameClass name_class, std::size_t max_length);

  [[nodiscard]] const std::string& str() const noexcept { return text_; }
  [[nodiscard]] std::string_view view() const noexcept { return text_; }
  [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
  [[nodiscard]] std::size_t size() const noexcept { return text_.size(); }

  friend bool operator==(const Name& lhs, const Name& rhs) noexcept { return lhs.text_ == rhs.text_; }
  friend std::strong_ordering operator<=>(const Name& lhs, const Name& rhs) noexcept {
    return lhs.text_ <=> rhs.text_;
  }

 private:
  explicit Name(std::string text) : text_(std::move(text)) {}
  std::string text_{};
};

// ---------------------------------------------------------------------------
// Tagged identifiers
//
// Each tag fixes the validation class and the maximum encoded length, so a
// persisted identifier can be rejected before it reaches any index.
// ---------------------------------------------------------------------------
struct AuthorityIdTag {
  static constexpr NameClass kClass = NameClass::authority;
  static constexpr std::size_t kMaxLength = 64;
  static constexpr std::string_view kLabel = "authority-id";
};
struct SourceIdTag {
  static constexpr NameClass kClass = NameClass::source;
  static constexpr std::size_t kMaxLength = 64;
  static constexpr std::string_view kLabel = "source-id";
};
struct EntityIdTag {
  static constexpr NameClass kClass = NameClass::entity;
  static constexpr std::size_t kMaxLength = 192;
  static constexpr std::string_view kLabel = "entity-id";
};
struct AspectIdTag {
  static constexpr NameClass kClass = NameClass::aspect;
  static constexpr std::size_t kMaxLength = 96;
  static constexpr std::string_view kLabel = "aspect-id";
};
struct TokenTag {
  static constexpr NameClass kClass = NameClass::token;
  static constexpr std::size_t kMaxLength = 48;
  static constexpr std::string_view kLabel = "token";
};

template <class Tag>
class [[nodiscard]] TaggedName {
 public:
  using tag_type = Tag;

  TaggedName() = default;

  static Outcome<TaggedName> parse(std::string_view text) {
    auto validated = Name::parse(text, Tag::kClass, Tag::kMaxLength);
    if (!validated) {
      return validated.template propagate<TaggedName>();
    }
    return Outcome<TaggedName>::ok(TaggedName(std::move(validated).value()));
  }

  // Same validation, but failures are reported as an encoding fault so that a
  // corrupt persisted identifier is never confused with bad caller input.
  static Outcome<TaggedName> decode(std::string_view text) {
    auto parsed = TaggedName::parse(text);
    if (!parsed) {
      std::string detail(Tag::kLabel);
      detail.append(" rejected: ");
      detail.append(to_string(parsed.reason().code));
      if (!parsed.reason().detail.empty()) {
        detail.append(" (");
        detail.append(parsed.reason().detail);
        detail.push_back(')');
      }
      return Outcome<TaggedName>::fail(ReasonCode::malformed_encoding, std::move(detail));
    }
    return parsed;
  }

  [[nodiscard]] const std::string& str() const noexcept { return name_.str(); }
  [[nodiscard]] std::string_view view() const noexcept { return name_.view(); }
  [[nodiscard]] bool empty() const noexcept { return name_.empty(); }

  friend bool operator==(const TaggedName& lhs, const TaggedName& rhs) noexcept {
    return lhs.name_ == rhs.name_;
  }
  friend std::strong_ordering operator<=>(const TaggedName& lhs, const TaggedName& rhs) noexcept {
    return lhs.name_ <=> rhs.name_;
  }

 private:
  explicit TaggedName(Name name) : name_(std::move(name)) {}
  Name name_{};
};

using AuthorityId = TaggedName<AuthorityIdTag>;
using SourceId = TaggedName<SourceIdTag>;
using EntityId = TaggedName<EntityIdTag>;
using AspectId = TaggedName<AspectIdTag>;
using Token = TaggedName<TokenTag>;

// ---------------------------------------------------------------------------
// Monotonic counters
//
// Zero means "unset / never published". Published epochs, generations and
// revisions start at one, so a default-constructed counter can never be
// mistaken for a real fence.
// ---------------------------------------------------------------------------
template <class Tag, class Rep = std::uint64_t>
class [[nodiscard]] Counter {
 public:
  using rep_type = Rep;
  using tag_type = Tag;

  constexpr Counter() noexcept = default;

  static constexpr Counter from_value(Rep value) noexcept { return Counter(value); }

  [[nodiscard]] constexpr Rep value() const noexcept { return value_; }
  [[nodiscard]] constexpr bool is_unset() const noexcept { return value_ == Rep{0}; }

  [[nodiscard]] Outcome<Counter> next() const {
    if (value_ == std::numeric_limits<Rep>::max()) {
      return Outcome<Counter>::fail(ReasonCode::arithmetic_overflow,
                                    std::string(Tag::kLabel) + " counter exhausted");
    }
    return Outcome<Counter>::ok(Counter(static_cast<Rep>(value_ + Rep{1})));
  }

  [[nodiscard]] constexpr std::uint64_t as_u64() const noexcept {
    return static_cast<std::uint64_t>(value_);
  }

  friend constexpr bool operator==(Counter lhs, Counter rhs) noexcept { return lhs.value_ == rhs.value_; }
  friend constexpr auto operator<=>(Counter lhs, Counter rhs) noexcept { return lhs.value_ <=> rhs.value_; }

 private:
  explicit constexpr Counter(Rep value) noexcept : value_(value) {}
  Rep value_{0};
};

struct EpochTag {
  static constexpr std::string_view kLabel = "epoch";
};
struct GenerationTag {
  static constexpr std::string_view kLabel = "generation";
};
struct RevisionTag {
  static constexpr std::string_view kLabel = "revision";
};
struct SequenceTag {
  static constexpr std::string_view kLabel = "sequence";
};

using Epoch = Counter<EpochTag>;
using Generation = Counter<GenerationTag>;
using Revision = Counter<RevisionTag>;
using JournalSequence = Counter<SequenceTag>;

// ---------------------------------------------------------------------------
// Units
// ---------------------------------------------------------------------------
enum class Unit : std::uint8_t {
  none = 0,
  count = 1,
  ratio = 2,
  percent = 3,
  parts_per_million = 4,
  bytes = 5,
  seconds = 6,
  watts = 7,
  kilowatts = 8,
  megawatts = 9,
  kilowatt_hours = 10,
  joules = 11,
  celsius = 12,
  kelvin = 13,
  volts = 14,
  amperes = 15,
  hertz = 16,
  pascals = 17,
  liters_per_second = 18,
  cubic_meters_per_second = 19,
  usd = 20,
};

FO_API std::string_view to_string(Unit unit) noexcept;
FO_API Outcome<Unit> unit_from_string(std::string_view text);
FO_API Unit canonical_unit(Unit unit) noexcept;
FO_API bool units_compatible(Unit lhs, Unit rhs) noexcept;
FO_API bool is_known_unit(Unit unit) noexcept;

// ---------------------------------------------------------------------------
// FixedPoint
//
// Exact decimal fixed point: int64 mantissa plus a scale in [0, 9]. Chosen over
// binary floating point so that comparisons, aggregations and persisted values
// are bit-for-bit reproducible and can never disagree across runs or machines.
// ---------------------------------------------------------------------------
enum class RoundingMode : std::uint8_t {
  reject_inexact = 0,
  half_even = 1,
  toward_zero = 2,
  floor = 3,
};

FO_API std::string_view to_string(RoundingMode mode) noexcept;

class FO_API FixedPoint {
 public:
  static constexpr int kMaxScale = 9;
  static constexpr int kMinScale = 0;

  constexpr FixedPoint() noexcept = default;

  static Outcome<FixedPoint> from_scaled(std::int64_t mantissa, int scale);

  // Strict parser: optional sign, at least one digit, optional fractional part
  // introduced by '.'. No exponent notation, no separators, no whitespace.
  static Outcome<FixedPoint> parse(std::string_view text);

  [[nodiscard]] constexpr std::int64_t mantissa() const noexcept { return mantissa_; }
  [[nodiscard]] constexpr int scale() const noexcept { return scale_; }
  [[nodiscard]] constexpr bool is_zero() const noexcept { return mantissa_ == 0; }
  [[nodiscard]] constexpr bool is_negative() const noexcept { return mantissa_ < 0; }

  [[nodiscard]] std::string to_string() const;

  // The smallest scale that still represents this value exactly. 4250.00 and
  // 4250 are the same number; only one of them is a canonical rendering.
  [[nodiscard]] FixedPoint reduced() const noexcept;

  // Exact canonical comparison. Fails only when a common scale cannot be
  // represented.
  [[nodiscard]] Outcome<std::strong_ordering> compare(const FixedPoint& other) const;

  [[nodiscard]] Outcome<FixedPoint> add(const FixedPoint& other) const;
  [[nodiscard]] Outcome<FixedPoint> subtract(const FixedPoint& other) const;
  [[nodiscard]] Outcome<FixedPoint> multiply(std::int64_t factor) const;
  [[nodiscard]] Outcome<FixedPoint> multiply(const FixedPoint& other) const;
  [[nodiscard]] Outcome<FixedPoint> divide(std::int64_t divisor, RoundingMode mode = RoundingMode::reject_inexact) const;
  [[nodiscard]] Outcome<FixedPoint> negated() const;
  [[nodiscard]] Outcome<FixedPoint> absolute() const;

  // Moves the value to a different scale. Grow always exact; shrink exact only
  // when every dropped digit is zero unless an explicit rounding mode is given.
  [[nodiscard]] Outcome<FixedPoint> rescale(int new_scale,
                                            RoundingMode mode = RoundingMode::reject_inexact) const;

  friend constexpr bool operator==(const FixedPoint& lhs, const FixedPoint& rhs) noexcept {
    return lhs.scale_ == rhs.scale_ && lhs.mantissa_ == rhs.mantissa_;
  }

 private:
  constexpr FixedPoint(std::int64_t mantissa, int scale) noexcept : mantissa_(mantissa), scale_(scale) {}

  // divides an exact (mantissa, scale) pair, growing the scale only as far as
  // it must in order to keep the quotient exact.
  static Outcome<FixedPoint> scale_divide(std::int64_t mantissa, int scale, std::int64_t divisor,
                                          RoundingMode mode);

  std::int64_t mantissa_{0};
  int scale_{0};
};

// ---------------------------------------------------------------------------
// Unit conversion
// ---------------------------------------------------------------------------
struct UnitInfo {
  Unit unit;
  std::string_view name;
  Unit canonical;
  std::int64_t numer;
  std::int64_t denom;
  std::int64_t offset_mantissa;
  int offset_scale;
};

FO_API const UnitInfo& unit_info(Unit unit) noexcept;

// Converts to the canonical unit of the dimension. Exact or it fails.
FO_API Outcome<FixedPoint> to_canonical(const FixedPoint& value, Unit from);

// Converts between any two units of the same dimension.
FO_API Outcome<FixedPoint> convert(const FixedPoint& value, Unit from, Unit to);

// Power-of-ten lookup that fails instead of overflowing.
FO_API Outcome<std::int64_t> pow10(int exponent);

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_STRONG_HPP
