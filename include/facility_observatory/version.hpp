// Facility Observatory - library version identity.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_VERSION_HPP
#define FACILITY_OBSERVATORY_VERSION_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "facility_observatory/export.hpp"

#define FO_VERSION_MAJOR 1
#define FO_VERSION_MINOR 0
#define FO_VERSION_PATCH 0

// The journal format version this build writes and can read back.
#define FO_JOURNAL_FORMAT_VERSION 1u

namespace fo {

struct SemanticVersion {
  std::uint32_t major{0};
  std::uint32_t minor{0};
  std::uint32_t patch{0};

  friend constexpr bool operator==(const SemanticVersion&, const SemanticVersion&) noexcept = default;
  friend constexpr auto operator<=>(const SemanticVersion&, const SemanticVersion&) noexcept = default;
};

// Compile-time version of the headers this translation unit compiled against.
FO_API const SemanticVersion& library_version() noexcept;

// Human readable form, e.g. "1.0.0".
FO_API std::string_view version_string() noexcept;

// A stable tag describing the ABI/behavioural contract of this build. Consumers
// persist it alongside evidence so that a mismatch can be reported rather than
// silently ignored.
FO_API std::string_view abi_tag() noexcept;

// Journal format version compiled into this build.
FO_API std::uint32_t journal_format_version() noexcept;

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_VERSION_HPP
