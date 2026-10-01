// Facility Observatory - integrity primitives.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_INTEGRITY_HPP
#define FACILITY_OBSERVATORY_INTEGRITY_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "facility_observatory/export.hpp"

namespace fo {

// CRC-32C (Castagnoli), reflected, polynomial 0x82F63B78. Used for every
// persisted frame so a torn or flipped byte is detected rather than absorbed.
FO_API std::uint32_t crc32c(const std::uint8_t* data, std::size_t size, std::uint32_t seed = 0) noexcept;
FO_API std::uint32_t crc32c(std::string_view text, std::uint32_t seed = 0) noexcept;

// FNV-1a 64. Used for deterministic content digests.
FO_API std::uint64_t fnv1a64(std::string_view text, std::uint64_t seed = 0xCBF29CE484222325ULL) noexcept;

// Lower-case, zero-padded hexadecimal.
FO_API std::string to_hex(std::uint32_t value);
FO_API std::string to_hex(std::uint64_t value);

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_INTEGRITY_HPP
