// Facility Observatory - strong type and checked arithmetic tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <limits>
#include <string>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/checked.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/strong.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

constexpr std::int64_t kInt64Max = std::numeric_limits<std::int64_t>::max();
constexpr std::int64_t kInt64Min = std::numeric_limits<std::int64_t>::min();

std::string fixed_to_string(const FixedPoint& value) { return value.to_string(); }

}  // namespace

FO_TEST(checked_add_boundaries) {
  std::int64_t out = 0;
  FO_REQUIRE(checked_add(kInt64Max - 1, 1, out));
  FO_REQUIRE_EQ(out, kInt64Max);
  FO_REQUIRE(!checked_add(kInt64Max, 1, out));
  FO_REQUIRE(!checked_add(kInt64Min, -1, out));
  FO_REQUIRE(checked_add(kInt64Min + 1, -1, out));
  FO_REQUIRE_EQ(out, kInt64Min);
  FO_REQUIRE(checked_add(kInt64Min, kInt64Max, out));
  FO_REQUIRE_EQ(out, -1);
}

FO_TEST(checked_sub_boundaries) {
  std::int64_t out = 0;
  FO_REQUIRE(checked_sub(kInt64Min, kInt64Min, out));
  FO_REQUIRE_EQ(out, 0);
  FO_REQUIRE(!checked_sub(0, kInt64Min, out));
  FO_REQUIRE(!checked_sub(kInt64Min, 1, out));
  FO_REQUIRE(checked_sub(kInt64Min + 1, 1, out));
  FO_REQUIRE_EQ(out, kInt64Min);
}

FO_TEST(checked_mul_boundaries) {
  std::int64_t out = 0;
  FO_REQUIRE(checked_mul(kInt64Min, 1, out));
  FO_REQUIRE_EQ(out, kInt64Min);
  FO_REQUIRE(checked_mul(kInt64Min, -1, out) == false);
  FO_REQUIRE(checked_mul(kInt64Min, 0, out));
  FO_REQUIRE_EQ(out, 0);
  FO_REQUIRE(checked_mul(-3037000499LL, 3037000499LL, out));
  FO_REQUIRE(!checked_mul(3037000500LL, 3037000500LL, out));
  FO_REQUIRE(checked_mul(kInt64Max, -1, out));
  FO_REQUIRE_EQ(out, -kInt64Max);
}

FO_TEST(checked_div_and_mod) {
  std::int64_t out = 0;
  FO_REQUIRE(!checked_div(1, 0, out));
  FO_REQUIRE(!checked_div(kInt64Min, -1, out));
  FO_REQUIRE(checked_div(kInt64Min, 1, out));
  FO_REQUIRE_EQ(out, kInt64Min);
  FO_REQUIRE(checked_mod(kInt64Min, -1, out));
  FO_REQUIRE_EQ(out, 0);
  FO_REQUIRE(checked_mod(-7, 3, out));
  FO_REQUIRE_EQ(out, -1);
}

FO_TEST(magnitude_is_total) {
  FO_REQUIRE_EQ(magnitude(kInt64Min), static_cast<std::uint64_t>(1) << 63);
  FO_REQUIRE_EQ(magnitude(-1), static_cast<std::uint64_t>(1));
  FO_REQUIRE_EQ(magnitude(kInt64Max), static_cast<std::uint64_t>(kInt64Max));
}

FO_TEST(checked_arithmetic_property) {
  Rng rng(0x5EED1234ULL);
  for (int iteration = 0; iteration < 20000; ++iteration) {
    const std::int64_t a = static_cast<std::int64_t>(rng.next());
    const std::int64_t b = static_cast<std::int64_t>(rng.next());
    std::int64_t sum = 0;
    if (checked_add(a, b, sum)) {
      std::int64_t back = 0;
      FO_REQUIRE(checked_sub(sum, a, back));
      FO_REQUIRE_EQ(back, b);
    } else if (b > 0) {
      FO_REQUIRE(a > kInt64Max - b);
    } else {
      FO_REQUIRE(a < kInt64Min - b);
    }

    std::int64_t product = 0;
    if (checked_mul(a, b, product)) {
      if (b != 0 && !(a == kInt64Min && b == -1)) {
        std::int64_t quotient = 0;
        FO_REQUIRE(checked_div(product, b, quotient));
        FO_REQUIRE_EQ(quotient, a);
      }
    }
  }
}

FO_TEST(fixed_point_parse_and_render_round_trip) {
  const char* literals[] = {"0", "1", "-1", "0.5", "-0.5", "123456789.123456789", "-0.000000001",
                            "9223372036854775807", "0.000000000"};
  for (const char* literal : literals) {
    auto parsed = FixedPoint::parse(literal);
    FO_REQUIRE_MESSAGE(parsed.has_value(), std::string("failed to parse ") + literal);
    FO_REQUIRE_EQ(parsed.value().to_string(), std::string(literal));
  }
}

FO_TEST(fixed_point_parse_rejections) {
  const char* rejected[] = {"",     ".",    ".5",   "1.",   "1.2.3", "1e3",  " 1",  "1 ",
                            "+",    "-",    "1,5",  "--1",  "00.1",  "01",   "00",  "-0",
                            "-0.0", "007",  "1.1234567890"};
  for (const char* literal : rejected) {
    FO_REQUIRE_MESSAGE(!FixedPoint::parse(literal).has_value(), std::string("accepted ") + literal);
  }
}

FO_TEST(fixed_point_scale_bounds) {
  FO_REQUIRE(FixedPoint::from_scaled(1, 0).has_value());
  FO_REQUIRE(FixedPoint::from_scaled(1, 9).has_value());
  FO_REQUIRE(!FixedPoint::from_scaled(1, -1).has_value());
  FO_REQUIRE(!FixedPoint::from_scaled(1, 10).has_value());
  auto smallest = FixedPoint::parse("0.000000001").value();
  FO_REQUIRE(!smallest.rescale(0).has_value());
  FO_REQUIRE(!smallest.rescale(-1).has_value());
}

FO_TEST(fixed_point_add_and_compare_are_consistent) {
  Rng rng(0xC0FFEEULL);
  for (int iteration = 0; iteration < 5000; ++iteration) {
    const std::int64_t a = rng.range(-1000000, 1000000);
    const std::int64_t b = rng.range(-1000000, 1000000);
    const int scale_a = static_cast<int>(rng.below(5));
    const int scale_b = static_cast<int>(rng.below(5));
    const FixedPoint left = FixedPoint::from_scaled(a, scale_a).value();
    const FixedPoint right = FixedPoint::from_scaled(b, scale_b).value();

    auto forward = left.add(right);
    auto backward = right.add(left);
    FO_REQUIRE(forward.has_value());
    FO_REQUIRE(backward.has_value());
    FO_REQUIRE_EQ(fixed_to_string(forward.value()), fixed_to_string(backward.value()));

    auto ordering = forward.value().compare(left);
    FO_REQUIRE(ordering.has_value());
    FO_REQUIRE(ordering.value() == (b < 0 ? std::strong_ordering::less
                                          : (b == 0 ? std::strong_ordering::equal : std::strong_ordering::greater)));
  }
}

FO_TEST(fixed_point_division_is_exact_or_fails) {
  auto half = FixedPoint::parse("1").value().divide(2);
  FO_REQUIRE(half.has_value());
  FO_REQUIRE_EQ(half.value().to_string(), std::string("0.5"));

  auto third = FixedPoint::parse("1").value().divide(3);
  FO_REQUIRE(!third.has_value());
  FO_REQUIRE(third.code() == ReasonCode::inexact_conversion);

  auto rounded = FixedPoint::parse("1").value().divide(3, RoundingMode::half_even);
  FO_REQUIRE(rounded.has_value());
  FO_REQUIRE_EQ(rounded.value().to_string(), std::string("0.333333333"));

  auto two_thirds = FixedPoint::parse("2").value().divide(3, RoundingMode::half_even);
  FO_REQUIRE(two_thirds.has_value());
  FO_REQUIRE_EQ(two_thirds.value().to_string(), std::string("0.666666667"));

  auto minus_one_third = FixedPoint::parse("-1").value().divide(3, RoundingMode::floor);
  FO_REQUIRE(minus_one_third.has_value());
  FO_REQUIRE_EQ(minus_one_third.value().to_string(), std::string("-0.333333334"));

  auto toward_zero = FixedPoint::parse("-1").value().divide(3, RoundingMode::toward_zero);
  FO_REQUIRE(toward_zero.has_value());
  FO_REQUIRE_EQ(toward_zero.value().to_string(), std::string("-0.333333333"));

  FO_REQUIRE(!FixedPoint::parse("1").value().divide(0).has_value());
}

FO_TEST(fixed_point_minimum_value_edges) {
  const FixedPoint minimum = FixedPoint::from_scaled(kInt64Min, 0).value();
  FO_REQUIRE(!minimum.negated().has_value());
  FO_REQUIRE(!minimum.absolute().has_value());
  FO_REQUIRE_EQ(minimum.to_string(), std::to_string(kInt64Min));
  auto halved = minimum.divide(2);
  FO_REQUIRE(halved.has_value());
  FO_REQUIRE_EQ(halved.value().to_string(), std::string("-4611686018427387904"));
}

FO_TEST(unit_conversion_is_exact) {
  auto value = FixedPoint::parse("1.5").value();
  auto watts = convert(value, Unit::kilowatts, Unit::watts);
  FO_REQUIRE(watts.has_value());
  FO_REQUIRE_EQ(watts.value().to_string(), std::string("1500.0"));

  auto back = convert(watts.value(), Unit::watts, Unit::kilowatts);
  FO_REQUIRE(back.has_value());
  FO_REQUIRE(back.value().compare(value).value() == std::strong_ordering::equal);

  auto megawatts = convert(value, Unit::kilowatts, Unit::megawatts);
  FO_REQUIRE(megawatts.has_value());
  FO_REQUIRE_EQ(megawatts.value().to_string(), std::string("0.0015"));

  auto percent = convert(FixedPoint::parse("50").value(), Unit::percent, Unit::ratio);
  FO_REQUIRE(percent.has_value());
  FO_REQUIRE_EQ(percent.value().to_string(), std::string("0.5"));

  auto kelvin = convert(FixedPoint::parse("25").value(), Unit::celsius, Unit::kelvin);
  FO_REQUIRE(kelvin.has_value());
  FO_REQUIRE_EQ(kelvin.value().to_string(), std::string("298.15"));

  auto celsius = convert(kelvin.value(), Unit::kelvin, Unit::celsius);
  FO_REQUIRE(celsius.has_value());
  FO_REQUIRE(celsius.value().compare(FixedPoint::parse("25").value()).value() == std::strong_ordering::equal);

  FO_REQUIRE(!convert(value, Unit::kilowatts, Unit::celsius).has_value());
  FO_REQUIRE(!units_compatible(Unit::watts, Unit::celsius));
  FO_REQUIRE(units_compatible(Unit::kilowatts, Unit::megawatts));
  FO_REQUIRE_EQ(std::string(to_string(canonical_unit(Unit::megawatts))), std::string("watts"));
}

FO_TEST(unit_conversion_round_trip_property) {
  Rng rng(0xBEEFULL);
  const Unit units[] = {Unit::watts, Unit::kilowatts, Unit::megawatts};
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::int64_t mantissa = rng.range(0, 1000000);
    const FixedPoint value = FixedPoint::from_scaled(mantissa, 3).value();
    const Unit from = units[rng.below(3)];
    const Unit to = units[rng.below(3)];
    auto converted = convert(value, from, to);
    FO_REQUIRE(converted.has_value());
    auto returned = convert(converted.value(), to, from);
    FO_REQUIRE(returned.has_value());
    auto ordering = returned.value().compare(value);
    FO_REQUIRE(ordering.has_value());
    FO_REQUIRE(ordering.value() == std::strong_ordering::equal);
  }
}

FO_TEST(utf8_validation) {
  FO_REQUIRE(is_valid_utf8("plain ascii"));
  FO_REQUIRE(is_valid_utf8("caf\xC3\xA9"));
  FO_REQUIRE(is_valid_utf8("\xE2\x82\xAC"));
  FO_REQUIRE(is_valid_utf8("\xF0\x9F\x9A\x80"));
  FO_REQUIRE(!is_valid_utf8("\xC0\xAF"));
  FO_REQUIRE(!is_valid_utf8("\xE0\x80\xAF"));
  FO_REQUIRE(!is_valid_utf8("\xED\xA0\x80"));
  FO_REQUIRE(!is_valid_utf8("\xF4\x90\x80\x80"));
  FO_REQUIRE(!is_valid_utf8("\xC3"));
  FO_REQUIRE(!is_valid_utf8("\x80"));
  FO_REQUIRE(!is_valid_utf8(std::string("a\0b", 3)));
  FO_REQUIRE(!is_valid_utf8("\xFF"));
}

FO_TEST(identifier_validation_classes) {
  FO_REQUIRE(AuthorityId::parse("dccp-a").has_value());
  FO_REQUIRE(AuthorityId::parse("A.B_C:D-1").has_value());
  FO_REQUIRE(!AuthorityId::parse("-leading").has_value());
  FO_REQUIRE(!AuthorityId::parse("has space").has_value());
  FO_REQUIRE(!AuthorityId::parse("").has_value());
  FO_REQUIRE(AuthorityId::parse(std::string(64, 'a')).has_value());
  auto too_long = AuthorityId::parse(std::string(65, 'a'));
  FO_REQUIRE(!too_long.has_value());
  FO_REQUIRE(too_long.code() == ReasonCode::identifier_too_long);

  FO_REQUIRE(AspectId::parse("power.draw").has_value());
  FO_REQUIRE(AspectId::parse("power").has_value());
  FO_REQUIRE(AspectId::parse("a.b.c_d.e9").has_value());
  FO_REQUIRE(!AspectId::parse("Power.draw").has_value());
  FO_REQUIRE(!AspectId::parse("power..draw").has_value());
  FO_REQUIRE(!AspectId::parse("power.").has_value());
  FO_REQUIRE(!AspectId::parse(".power").has_value());
  FO_REQUIRE(!AspectId::parse("power-draw").has_value());

  FO_REQUIRE(EntityId::parse("sea1-r07").has_value());
  FO_REQUIRE(EntityId::parse("caf\xC3\xA9").has_value());
  FO_REQUIRE(!EntityId::parse("bad\xC3").has_value());
  FO_REQUIRE(EntityId::parse(std::string(192, 'x')).has_value());
  FO_REQUIRE(!EntityId::parse(std::string(193, 'x')).has_value());
}

FO_TEST(recovered_identifiers_report_encoding_faults) {
  auto decoded = AuthorityId::decode("has space");
  FO_REQUIRE(!decoded.has_value());
  FO_REQUIRE(decoded.code() == ReasonCode::malformed_encoding);
}

FO_TEST(counter_overflow_is_refused) {
  auto maximum = Epoch::from_value(std::numeric_limits<std::uint64_t>::max());
  FO_REQUIRE(!maximum.next().has_value());
  FO_REQUIRE(Epoch::from_value(0).is_unset());
  FO_REQUIRE(Epoch::from_value(1).next().value().value() == 2);
  FO_REQUIRE(Epoch{}.is_unset());
}

FO_TEST(entity_reference_parsing) {
  auto entity = EntityRef::parse("rack:sea1-r07");
  FO_REQUIRE(entity.has_value());
  FO_REQUIRE_EQ(entity.value().to_string(), std::string("rack:sea1-r07"));
  FO_REQUIRE(entity.value().domain == Domain::rack);

  FO_REQUIRE(!EntityRef::parse("sea1-r07").has_value());
  FO_REQUIRE(!EntityRef::parse("unknown:x").has_value());
  FO_REQUIRE(EntityRef::parse("site:").has_value() == false);

  auto namespaced = EntityRef::parse("asset:vendor:model:42");
  FO_REQUIRE(namespaced.has_value());
  FO_REQUIRE_EQ(namespaced.value().id.str(), std::string("vendor:model:42"));
}

FO_TEST(boundary_operations_are_refused) {
  FO_REQUIRE(assert_observer_operation(BoundaryOperation::read_evidence).has_value());
  FO_REQUIRE(assert_observer_operation(BoundaryOperation::record_evidence).has_value());
  auto refused = assert_observer_operation(BoundaryOperation::command_facility_state);
  FO_REQUIRE(!refused.has_value());
  FO_REQUIRE(refused.code() == ReasonCode::mutation_refused);
  auto cooling = assert_observer_operation(BoundaryOperation::control_cooling);
  FO_REQUIRE(!cooling.has_value());
  FO_REQUIRE(cooling.code() == ReasonCode::mutation_refused);
  FO_REQUIRE(!is_observer_operation(BoundaryOperation::place_workload));
  FO_REQUIRE(!systems_boundary_statement().empty());
}
