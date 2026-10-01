// Facility Observatory - deterministic JSON.
//
// Object members are emitted in insertion order, integers are exact, and there
// is no floating-point representation anywhere in the wire format: decimal
// quantities travel as strings so that a value can never be silently rounded by
// a JSON layer.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_JSON_HPP
#define FACILITY_OBSERVATORY_JSON_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"

namespace fo {

class FO_API JsonValue {
 public:
  enum class Type : std::uint8_t {
    null_value = 0,
    boolean = 1,
    integer = 2,
    string = 3,
    array = 4,
    object = 5,
  };

  static constexpr std::size_t kMaxDepth = 32;
  static constexpr std::size_t kMaxStringBytes = 1U << 20;

  JsonValue() = default;

  static JsonValue make_null();
  static JsonValue make_boolean(bool value);
  static JsonValue make_integer(std::int64_t value);
  static JsonValue make_string(std::string value);
  static JsonValue make_array();
  static JsonValue make_object();

  [[nodiscard]] Type type() const noexcept { return type_; }
  [[nodiscard]] bool is_null() const noexcept { return type_ == Type::null_value; }
  [[nodiscard]] bool is_object() const noexcept { return type_ == Type::object; }
  [[nodiscard]] bool is_array() const noexcept { return type_ == Type::array; }
  [[nodiscard]] bool is_string() const noexcept { return type_ == Type::string; }

  void push_back(JsonValue value);
  // A repeated key replaces the existing value in place, preserving position.
  void set(std::string key, JsonValue value);

  [[nodiscard]] bool contains(std::string_view key) const;
  [[nodiscard]] const JsonValue* find(std::string_view key) const;
  [[nodiscard]] std::string_view string_or(std::string_view fallback) const noexcept;
  [[nodiscard]] std::int64_t integer_or(std::int64_t fallback) const noexcept;
  [[nodiscard]] bool boolean_or(bool fallback) const noexcept;

  [[nodiscard]] const std::vector<JsonValue>& items() const noexcept { return items_; }
  [[nodiscard]] const std::vector<std::pair<std::string, JsonValue>>& members() const noexcept {
    return members_;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return type_ == Type::object ? members_.size() : items_.size();
  }

  // indent == 0 produces a single compact line. No trailing newline is added.
  [[nodiscard]] std::string dump(int indent = 0) const;

  static Outcome<JsonValue> parse(std::string_view text, std::size_t max_bytes = (16U << 20));

 private:
  void dump_into(std::string& out, int indent, std::size_t depth) const;

  Type type_{Type::null_value};
  bool boolean_{false};
  std::int64_t integer_{0};
  std::string string_{};
  std::vector<JsonValue> items_{};
  std::vector<std::pair<std::string, JsonValue>> members_{};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_JSON_HPP
