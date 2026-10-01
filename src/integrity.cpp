// Facility Observatory - integrity primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/integrity.hpp"

#include <array>

namespace fo {
namespace {

constexpr std::array<std::uint32_t, 256> make_crc32c_table() noexcept {
  std::array<std::uint32_t, 256> table{};
  for (std::uint32_t index = 0; index < 256U; ++index) {
    std::uint32_t crc = index;
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1) ^ (0x82F63B78U & mask);
    }
    table[index] = crc;
  }
  return table;
}

constexpr std::array<std::uint32_t, 256> kCrc32cTable = make_crc32c_table();

constexpr char kHexDigits[] = "0123456789abcdef";

template <class Unsigned>
std::string to_hex_padded(Unsigned value, std::size_t digits) {
  std::string result(digits, '0');
  for (std::size_t index = 0; index < digits; ++index) {
    const std::size_t shift = (digits - 1 - index) * 4U;
    result[index] = kHexDigits[static_cast<std::size_t>((value >> shift) & static_cast<Unsigned>(0xFU))];
  }
  return result;
}

}  // namespace

std::uint32_t crc32c(const std::uint8_t* data, std::size_t size, std::uint32_t seed) noexcept {
  std::uint32_t crc = seed ^ 0xFFFFFFFFU;
  for (std::size_t index = 0; index < size; ++index) {
    const std::uint8_t byte = data[index];
    crc = kCrc32cTable[(crc ^ byte) & 0xFFU] ^ (crc >> 8);
  }
  return crc ^ 0xFFFFFFFFU;
}

std::uint32_t crc32c(std::string_view text, std::uint32_t seed) noexcept {
  return crc32c(reinterpret_cast<const std::uint8_t*>(text.data()), text.size(), seed);
}

std::uint64_t fnv1a64(std::string_view text, std::uint64_t seed) noexcept {
  std::uint64_t hash = seed;
  for (const char c : text) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(c));
    hash *= 0x100000001B3ULL;
  }
  return hash;
}

std::string to_hex(std::uint32_t value) { return to_hex_padded(value, 8); }

std::string to_hex(std::uint64_t value) { return to_hex_padded(value, 16); }

}  // namespace fo
