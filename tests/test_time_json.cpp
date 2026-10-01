// Facility Observatory - time and JSON tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <limits>
#include <string>

#include "facility_observatory/json.hpp"
#include "facility_observatory/time.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

FO_TEST(rfc3339_round_trip) {
  const char* stamps[] = {"1970-01-01T00:00:00Z",
                          "2026-03-01T12:34:56.789Z",
                          "2026-03-01T12:34:56.000000001Z",
                          "1969-12-31T23:59:59Z",
                          "2000-02-29T00:00:00Z",
                          "2024-12-31T23:59:59.999999999Z",
                          "1900-01-01T00:00:00Z"};
  for (const char* text : stamps) {
    auto parsed = Timestamp::parse_rfc3339(text);
    FO_REQUIRE_MESSAGE(parsed.has_value(), std::string("failed to parse ") + text);
    FO_REQUIRE_EQ(parsed.value().to_rfc3339(), std::string(text));
  }
}

FO_TEST(rfc3339_offsets_normalise_to_utc) {
  auto positive = Timestamp::parse_rfc3339("2026-03-01T14:34:56+02:00");
  FO_REQUIRE(positive.has_value());
  FO_REQUIRE_EQ(positive.value().to_rfc3339(), std::string("2026-03-01T12:34:56Z"));

  auto negative = Timestamp::parse_rfc3339("2026-03-01T07:04:56-05:30");
  FO_REQUIRE(negative.has_value());
  FO_REQUIRE_EQ(negative.value().to_rfc3339(), std::string("2026-03-01T12:34:56Z"));
}

FO_TEST(rfc3339_fraction_rendering_is_adaptive) {
  auto millis = Timestamp::parse_rfc3339("2026-03-01T12:34:56.789Z").value();
  FO_REQUIRE_EQ(millis.to_rfc3339(), std::string("2026-03-01T12:34:56.789Z"));
  auto micros = Timestamp::parse_rfc3339("2026-03-01T12:34:56.789123Z").value();
  FO_REQUIRE_EQ(micros.to_rfc3339(), std::string("2026-03-01T12:34:56.789123Z"));
  auto nanos = Timestamp::parse_rfc3339("2026-03-01T12:34:56.789123456Z").value();
  FO_REQUIRE_EQ(nanos.to_rfc3339(), std::string("2026-03-01T12:34:56.789123456Z"));
  auto whole = Timestamp::parse_rfc3339("2026-03-01T12:34:56Z").value();
  FO_REQUIRE_EQ(whole.to_rfc3339(), std::string("2026-03-01T12:34:56Z"));
}

FO_TEST(rfc3339_rejections) {
  const char* rejected[] = {"",
                            "2026-03-01",
                            "2026-03-01T12:34:56",
                            "2026-03-01t12:34:56Z",
                            "2026-03-01T12:34:56z",
                            "2026-03-01T12:34:60Z",
                            "2026-13-01T12:34:56Z",
                            "2026-00-01T12:34:56Z",
                            "2026-03-32T12:34:56Z",
                            "2026-03-01T24:00:00Z",
                            "2026-03-01T12:60:00Z",
                            "2026-03-01T12:34:56.Z",
                            "2026-03-01T12:34:56.1234567890Z",
                            "2026-03-01T12:34:56Z ",
                            "2026-03-01T12:34:56+2:00",
                            "2026/03/01T12:34:56Z",
                            "20260301T123456Z"};
  for (const char* text : rejected) {
    FO_REQUIRE_MESSAGE(!Timestamp::parse_rfc3339(text).has_value(), std::string("accepted ") + text);
  }
}

FO_TEST(timestamp_unset_is_not_an_instant) {
  Timestamp unset;
  FO_REQUIRE(!unset.is_set());
  FO_REQUIRE_EQ(unset.to_rfc3339(), std::string("unset"));
  FO_REQUIRE(!Timestamp::from_unix_nanos(Timestamp::kUnsetValue).has_value());
  FO_REQUIRE(unset < Timestamp::from_unix_nanos(0).value());
  FO_REQUIRE(!duration_between(unset, Timestamp::from_unix_nanos(0).value()).has_value());
}

FO_TEST(duration_literals_round_trip) {
  const char* literals[] = {"0ms", "1ms", "500us", "999ns", "30s", "5m", "2h", "1d"};
  for (const char* literal : literals) {
    auto parsed = parse_duration(literal);
    FO_REQUIRE_MESSAGE(parsed.has_value(), std::string("failed to parse ") + literal);
    FO_REQUIRE_EQ(to_string(parsed.value()), std::string(literal));
  }
  // Multi-term literals are accepted but render in the single largest exact unit.
  auto composed = parse_duration("1h30m");
  FO_REQUIRE(composed.has_value());
  FO_REQUIRE_EQ(to_string(composed.value()), std::string("90m"));
  FO_REQUIRE(!parse_duration("").has_value());
  FO_REQUIRE(!parse_duration("10").has_value());
  FO_REQUIRE(!parse_duration("s").has_value());
  FO_REQUIRE(!parse_duration("-5s").has_value());
  FO_REQUIRE(!parse_duration("99999999999999999999d").has_value());
}

FO_TEST(duration_arithmetic_saturates) {
  const Duration maximum = Duration::max();
  const Duration minimum = Duration::min();
  FO_REQUIRE_EQ(saturating_add(maximum, Duration(1)).count(), maximum.count());
  FO_REQUIRE_EQ(saturating_sub(minimum, Duration(1)).count(), minimum.count());
  FO_REQUIRE_EQ(saturating_add(Duration(5), Duration(7)).count(), 12);
}

FO_TEST(manual_clock_is_monotonic_and_deterministic) {
  auto start = Timestamp::parse_rfc3339("2026-01-01T00:00:00Z").value();
  ManualClock clock(start);
  FO_REQUIRE_EQ(clock.now().to_rfc3339(), std::string("2026-01-01T00:00:00Z"));
  FO_REQUIRE(clock.advance(std::chrono::seconds(90)).has_value());
  FO_REQUIRE_EQ(clock.now().to_rfc3339(), std::string("2026-01-01T00:01:30Z"));
  FO_REQUIRE(!clock.advance(std::chrono::seconds(-1)).has_value());
  clock.set(start);
  FO_REQUIRE_EQ(clock.now().to_rfc3339(), std::string("2026-01-01T00:00:00Z"));
  clock.set(Timestamp{});
  FO_REQUIRE_EQ(clock.now().to_rfc3339(), std::string("2026-01-01T00:00:00Z"));
}

FO_TEST(json_round_trip) {
  const char* documents[] = {
      "{}",
      "[]",
      "null",
      "true",
      "false",
      "0",
      "-17",
      "\"text\"",
      "{\"a\":1,\"b\":[1,2,3],\"c\":{\"d\":\"e\"}}",
      "{\"escaped\":\"quote \\\" backslash \\\\ newline \\n tab \\t\"}",
      "{\"unicode\":\"\\u20ac\"}",
      "{\"surrogate\":\"\\ud83d\\ude00\"}",
      "{\"nested\":[[[[1]]]]}",
  };
  for (const char* text : documents) {
    auto parsed = JsonValue::parse(text);
    FO_REQUIRE_MESSAGE(parsed.has_value(), std::string("failed to parse ") + text);
    const std::string dumped = parsed.value().dump();
    auto reparsed = JsonValue::parse(dumped);
    FO_REQUIRE_MESSAGE(reparsed.has_value(), std::string("failed to reparse ") + dumped);
    FO_REQUIRE_EQ(reparsed.value().dump(), dumped);
  }
}

FO_TEST(json_escapes_control_characters) {
  JsonValue object = JsonValue::make_object();
  object.set("control", JsonValue::make_string(std::string("a\x01\x1F b")));
  const std::string dumped = object.dump();
  FO_REQUIRE_EQ(dumped, std::string("{\"control\":\"a\\u0001\\u001f b\"}"));
  auto reparsed = JsonValue::parse(dumped);
  FO_REQUIRE(reparsed.has_value());
  FO_REQUIRE_EQ(std::string(reparsed.value().find("control")->string_or("")), std::string("a\x01\x1F b"));
}

FO_TEST(json_object_order_is_insertion_order) {
  JsonValue object = JsonValue::make_object();
  object.set("z", JsonValue::make_integer(1));
  object.set("a", JsonValue::make_integer(2));
  object.set("m", JsonValue::make_integer(3));
  FO_REQUIRE_EQ(object.dump(), std::string("{\"z\":1,\"a\":2,\"m\":3}"));
  object.set("a", JsonValue::make_integer(9));
  FO_REQUIRE_EQ(object.dump(), std::string("{\"z\":1,\"a\":9,\"m\":3}"));
}

FO_TEST(json_rejections) {
  const char* rejected[] = {"",
                            "{",
                            "}",
                            "[1,]",
                            "{\"a\":1,}",
                            "{\"a\"1}",
                            "{a:1}",
                            "01",
                            "-01",
                            "1.5",
                            "1e5",
                            "\"unterminated",
                            "\"bad \\q escape\"",
                            "tru",
                            "nul",
                            "\"raw\x01control\"",
                            "{} {}",
                            "+1",
                            "9223372036854775808",
                            "-9223372036854775809"};
  for (const char* text : rejected) {
    FO_REQUIRE_MESSAGE(!JsonValue::parse(text).has_value(), std::string("accepted ") + text);
  }
}

FO_TEST(json_rejects_excessive_depth) {
  std::string deep;
  for (int index = 0; index < 40; ++index) {
    deep.push_back('[');
  }
  deep.append("1");
  for (int index = 0; index < 40; ++index) {
    deep.push_back(']');
  }
  auto parsed = JsonValue::parse(deep);
  FO_REQUIRE(!parsed.has_value());
  FO_REQUIRE(parsed.code() == ReasonCode::malformed_encoding);
}

FO_TEST(json_rejects_oversized_documents) {
  const std::string big(1000, 'x');
  auto parsed = JsonValue::parse("\"" + big + "\"", 100);
  FO_REQUIRE(!parsed.has_value());
  FO_REQUIRE(parsed.code() == ReasonCode::record_too_large);
}

FO_TEST(json_parser_property) {
  Rng rng(0x1CEB00DAULL);
  const char* alphabet = "abc\"\\\n\t\x01 0";
  for (int iteration = 0; iteration < 2000; ++iteration) {
    const std::size_t length = static_cast<std::size_t>(rng.below(24));
    std::string text;
    for (std::size_t index = 0; index < length; ++index) {
      text.push_back(alphabet[rng.below(9)]);
    }
    auto parsed = JsonValue::parse(text);
    if (!parsed.has_value()) {
      continue;  // rejection is an acceptable outcome; a crash or hang is not
    }
    auto reparsed = JsonValue::parse(parsed.value().dump());
    FO_REQUIRE(reparsed.has_value());
    FO_REQUIRE_EQ(reparsed.value().dump(), parsed.value().dump());
  }
}
