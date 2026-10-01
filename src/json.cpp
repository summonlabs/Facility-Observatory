// Facility Observatory - deterministic JSON.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/json.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

#include "facility_observatory/checked.hpp"

namespace fo {
namespace {

void append_escaped(std::string& out, std::string_view text) {
  out.push_back('"');
  for (const char c : text) {
    const auto byte = static_cast<unsigned char>(c);
    switch (c) {
      case '"':
        out.append("\\\"");
        break;
      case '\\':
        out.append("\\\\");
        break;
      case '\b':
        out.append("\\b");
        break;
      case '\f':
        out.append("\\f");
        break;
      case '\n':
        out.append("\\n");
        break;
      case '\r':
        out.append("\\r");
        break;
      case '\t':
        out.append("\\t");
        break;
      default:
        if (byte < 0x20U) {
          constexpr char kHex[] = "0123456789abcdef";
          out.append("\\u00");
          out.push_back(kHex[(byte >> 4) & 0x0FU]);
          out.push_back(kHex[byte & 0x0FU]);
        } else {
          out.push_back(c);
        }
        break;
    }
  }
  out.push_back('"');
}

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  Outcome<JsonValue> parse_document() {
    skip_whitespace();
    auto value = parse_value(0);
    if (!value) {
      return value;
    }
    skip_whitespace();
    if (cursor_ != text_.size()) {
      return fail("trailing content after the top-level value");
    }
    return value;
  }

 private:
  Outcome<JsonValue> fail(std::string detail) const {
    return Outcome<JsonValue>::fail(ReasonCode::malformed_encoding,
                                    std::move(detail) + " at offset " + std::to_string(cursor_));
  }

  void skip_whitespace() {
    while (cursor_ < text_.size()) {
      const char c = text_[cursor_];
      if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
        ++cursor_;
        continue;
      }
      break;
    }
  }

  bool consume(char expected) {
    if (cursor_ < text_.size() && text_[cursor_] == expected) {
      ++cursor_;
      return true;
    }
    return false;
  }

  bool consume_literal(std::string_view literal) {
    if (text_.compare(cursor_, literal.size(), literal) == 0) {
      cursor_ += literal.size();
      return true;
    }
    return false;
  }

  Outcome<JsonValue> parse_value(std::size_t depth) {
    if (depth > JsonValue::kMaxDepth) {
      return fail("nesting exceeds the supported depth of " + std::to_string(JsonValue::kMaxDepth));
    }
    if (cursor_ >= text_.size()) {
      return fail("unexpected end of input");
    }
    const char c = text_[cursor_];
    switch (c) {
      case '{':
        return parse_object(depth);
      case '[':
        return parse_array(depth);
      case '"': {
        auto text = parse_string();
        if (!text) {
          return text.propagate<JsonValue>();
        }
        return Outcome<JsonValue>::ok(JsonValue::make_string(std::move(text).value()));
      }
      case 't':
        if (consume_literal("true")) {
          return Outcome<JsonValue>::ok(JsonValue::make_boolean(true));
        }
        return fail("invalid literal");
      case 'f':
        if (consume_literal("false")) {
          return Outcome<JsonValue>::ok(JsonValue::make_boolean(false));
        }
        return fail("invalid literal");
      case 'n':
        if (consume_literal("null")) {
          return Outcome<JsonValue>::ok(JsonValue::make_null());
        }
        return fail("invalid literal");
      default:
        return parse_integer();
    }
  }

  Outcome<JsonValue> parse_integer() {
    const bool negative = consume('-');
    if (cursor_ >= text_.size() || text_[cursor_] < '0' || text_[cursor_] > '9') {
      return fail("expected a digit");
    }
    if (text_[cursor_] == '0' && cursor_ + 1 < text_.size() && text_[cursor_ + 1] >= '0' &&
        text_[cursor_ + 1] <= '9') {
      return fail("leading zeros are not permitted");
    }
    std::int64_t magnitude = 0;
    while (cursor_ < text_.size() && text_[cursor_] >= '0' && text_[cursor_] <= '9') {
      std::int64_t shifted = 0;
      if (!checked_mul(magnitude, 10, shifted) ||
          !checked_add(shifted, static_cast<std::int64_t>(text_[cursor_] - '0'), magnitude)) {
        return fail("integer is outside the representable range");
      }
      ++cursor_;
    }
    if (cursor_ < text_.size() && (text_[cursor_] == '.' || text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
      return fail("fractional and exponent numbers are not part of this wire format; send decimals as strings");
    }
    if (negative) {
      if (magnitude == 0) {
        return Outcome<JsonValue>::ok(JsonValue::make_integer(0));
      }
      std::int64_t value = 0;
      if (!checked_sub(0, magnitude, value)) {
        return fail("integer is outside the representable range");
      }
      return Outcome<JsonValue>::ok(JsonValue::make_integer(value));
    }
    return Outcome<JsonValue>::ok(JsonValue::make_integer(magnitude));
  }

  Outcome<std::string> parse_string() {
    if (!consume('"')) {
      return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "expected a string");
    }
    std::string out;
    while (true) {
      if (cursor_ >= text_.size()) {
        return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "unterminated string");
      }
      const char c = text_[cursor_];
      if (c == '"') {
        ++cursor_;
        return Outcome<std::string>::ok(std::move(out));
      }
      if (c == '\\') {
        ++cursor_;
        if (cursor_ >= text_.size()) {
          return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "unterminated escape");
        }
        const char escaped = text_[cursor_++];
        switch (escaped) {
          case '"':
            out.push_back('"');
            break;
          case '\\':
            out.push_back('\\');
            break;
          case '/':
            out.push_back('/');
            break;
          case 'b':
            out.push_back('\b');
            break;
          case 'f':
            out.push_back('\f');
            break;
          case 'n':
            out.push_back('\n');
            break;
          case 'r':
            out.push_back('\r');
            break;
          case 't':
            out.push_back('\t');
            break;
          case 'u': {
            unsigned code_point = 0;
            auto read_hex4 = [this](unsigned& target) -> bool {
              if (cursor_ + 4 > text_.size()) {
                return false;
              }
              unsigned value = 0;
              for (int index = 0; index < 4; ++index) {
                const char digit = text_[cursor_ + static_cast<std::size_t>(index)];
                unsigned nibble = 0;
                if (digit >= '0' && digit <= '9') {
                  nibble = static_cast<unsigned>(digit - '0');
                } else if (digit >= 'a' && digit <= 'f') {
                  nibble = static_cast<unsigned>(digit - 'a') + 10U;
                } else if (digit >= 'A' && digit <= 'F') {
                  nibble = static_cast<unsigned>(digit - 'A') + 10U;
                } else {
                  return false;
                }
                value = (value << 4) | nibble;
              }
              cursor_ += 4;
              target = value;
              return true;
            };
            if (!read_hex4(code_point)) {
              return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "invalid \\u escape");
            }
            if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
              if (cursor_ + 2 > text_.size() || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
                return Outcome<std::string>::fail(ReasonCode::malformed_encoding,
                                                  "high surrogate without a low surrogate");
              }
              cursor_ += 2;
              unsigned low = 0;
              if (!read_hex4(low)) {
                return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "invalid \\u escape");
              }
              if (low < 0xDC00U || low > 0xDFFFU) {
                return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "invalid surrogate pair");
              }
              code_point = 0x10000U + ((code_point - 0xD800U) << 10) + (low - 0xDC00U);
            } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
              return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "stray low surrogate");
            }
            if (code_point < 0x80U) {
              out.push_back(static_cast<char>(code_point));
            } else if (code_point < 0x800U) {
              out.push_back(static_cast<char>(0xC0U | (code_point >> 6)));
              out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            } else if (code_point < 0x10000U) {
              out.push_back(static_cast<char>(0xE0U | (code_point >> 12)));
              out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
              out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            } else {
              out.push_back(static_cast<char>(0xF0U | (code_point >> 18)));
              out.push_back(static_cast<char>(0x80U | ((code_point >> 12) & 0x3FU)));
              out.push_back(static_cast<char>(0x80U | ((code_point >> 6) & 0x3FU)));
              out.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
            break;
          }
          default:
            return Outcome<std::string>::fail(ReasonCode::malformed_encoding, "unknown escape sequence");
        }
        if (out.size() > JsonValue::kMaxStringBytes) {
          return Outcome<std::string>::fail(ReasonCode::record_too_large, "string exceeds the supported length");
        }
        continue;
      }
      const auto byte = static_cast<unsigned char>(c);
      if (byte < 0x20U) {
        return Outcome<std::string>::fail(ReasonCode::malformed_encoding,
                                          "unescaped control character inside a string");
      }
      out.push_back(c);
      ++cursor_;
      if (out.size() > JsonValue::kMaxStringBytes) {
        return Outcome<std::string>::fail(ReasonCode::record_too_large, "string exceeds the supported length");
      }
    }
  }

  Outcome<JsonValue> parse_object(std::size_t depth) {
    if (!consume('{')) {
      return fail("expected '{'");
    }
    JsonValue object = JsonValue::make_object();
    skip_whitespace();
    if (consume('}')) {
      return Outcome<JsonValue>::ok(std::move(object));
    }
    while (true) {
      skip_whitespace();
      auto key = parse_string();
      if (!key) {
        return key.propagate<JsonValue>();
      }
      skip_whitespace();
      if (!consume(':')) {
        return fail("expected ':' after an object key");
      }
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      object.set(std::move(key).value(), std::move(value).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return Outcome<JsonValue>::ok(std::move(object));
      }
      return fail("expected ',' or '}' in an object");
    }
  }

  Outcome<JsonValue> parse_array(std::size_t depth) {
    if (!consume('[')) {
      return fail("expected '['");
    }
    JsonValue array = JsonValue::make_array();
    skip_whitespace();
    if (consume(']')) {
      return Outcome<JsonValue>::ok(std::move(array));
    }
    while (true) {
      skip_whitespace();
      auto value = parse_value(depth + 1);
      if (!value) {
        return value;
      }
      array.push_back(std::move(value).value());
      skip_whitespace();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return Outcome<JsonValue>::ok(std::move(array));
      }
      return fail("expected ',' or ']' in an array");
    }
  }

  std::string_view text_{};
  std::size_t cursor_{0};
};

}  // namespace

JsonValue JsonValue::make_null() { return JsonValue{}; }

JsonValue JsonValue::make_boolean(bool value) {
  JsonValue result;
  result.type_ = Type::boolean;
  result.boolean_ = value;
  return result;
}

JsonValue JsonValue::make_integer(std::int64_t value) {
  JsonValue result;
  result.type_ = Type::integer;
  result.integer_ = value;
  return result;
}

JsonValue JsonValue::make_string(std::string value) {
  JsonValue result;
  result.type_ = Type::string;
  result.string_ = std::move(value);
  return result;
}

JsonValue JsonValue::make_array() {
  JsonValue result;
  result.type_ = Type::array;
  return result;
}

JsonValue JsonValue::make_object() {
  JsonValue result;
  result.type_ = Type::object;
  return result;
}

void JsonValue::push_back(JsonValue value) { items_.push_back(std::move(value)); }

void JsonValue::set(std::string key, JsonValue value) {
  for (auto& member : members_) {
    if (member.first == key) {
      member.second = std::move(value);
      return;
    }
  }
  members_.emplace_back(std::move(key), std::move(value));
}

bool JsonValue::contains(std::string_view key) const { return find(key) != nullptr; }

const JsonValue* JsonValue::find(std::string_view key) const {
  for (const auto& member : members_) {
    if (member.first == key) {
      return &member.second;
    }
  }
  return nullptr;
}

std::string_view JsonValue::string_or(std::string_view fallback) const noexcept {
  return type_ == Type::string ? std::string_view(string_) : fallback;
}

std::int64_t JsonValue::integer_or(std::int64_t fallback) const noexcept {
  return type_ == Type::integer ? integer_ : fallback;
}

bool JsonValue::boolean_or(bool fallback) const noexcept {
  return type_ == Type::boolean ? boolean_ : fallback;
}

std::string JsonValue::dump(int indent) const {
  std::string out;
  dump_into(out, indent, 0);
  return out;
}

void JsonValue::dump_into(std::string& out, int indent, std::size_t depth) const {
  const bool pretty = indent > 0;
  const auto newline_indent = [&out, indent, pretty](std::size_t level) {
    if (!pretty) {
      return;
    }
    out.push_back('\n');
    out.append(level * static_cast<std::size_t>(indent), ' ');
  };
  switch (type_) {
    case Type::null_value:
      out.append("null");
      return;
    case Type::boolean:
      out.append(boolean_ ? "true" : "false");
      return;
    case Type::integer:
      out.append(std::to_string(integer_));
      return;
    case Type::string:
      append_escaped(out, string_);
      return;
    case Type::array: {
      if (items_.empty()) {
        out.append("[]");
        return;
      }
      out.push_back('[');
      for (std::size_t index = 0; index < items_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        newline_indent(depth + 1);
        items_[index].dump_into(out, indent, depth + 1);
      }
      newline_indent(depth);
      out.push_back(']');
      return;
    }
    case Type::object: {
      if (members_.empty()) {
        out.append("{}");
        return;
      }
      out.push_back('{');
      for (std::size_t index = 0; index < members_.size(); ++index) {
        if (index != 0) {
          out.push_back(',');
        }
        newline_indent(depth + 1);
        append_escaped(out, members_[index].first);
        out.push_back(':');
        if (pretty) {
          out.push_back(' ');
        }
        members_[index].second.dump_into(out, indent, depth + 1);
      }
      newline_indent(depth);
      out.push_back('}');
      return;
    }
  }
}

Outcome<JsonValue> JsonValue::parse(std::string_view text, std::size_t max_bytes) {
  if (text.size() > max_bytes) {
    return Outcome<JsonValue>::fail(ReasonCode::record_too_large,
                                    "document of " + std::to_string(text.size()) +
                                        " bytes exceeds the bound of " + std::to_string(max_bytes) + " bytes");
  }
  Parser parser(text);
  return parser.parse_document();
}

}  // namespace fo
