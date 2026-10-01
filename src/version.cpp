// Facility Observatory - library version identity.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/version.hpp"

namespace fo {
namespace {

constexpr SemanticVersion kVersion{FO_VERSION_MAJOR, FO_VERSION_MINOR, FO_VERSION_PATCH};

}  // namespace

const SemanticVersion& library_version() noexcept { return kVersion; }

std::string_view version_string() noexcept { return std::string_view{"1.0.0"}; }

std::string_view abi_tag() noexcept { return std::string_view{"facility-observatory-1.0"}; }

std::uint32_t journal_format_version() noexcept { return FO_JOURNAL_FORMAT_VERSION; }

}  // namespace fo
