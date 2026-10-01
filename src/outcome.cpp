// Facility Observatory - deterministic outcome and reason model.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/outcome.hpp"

namespace fo {
namespace {

struct CodeName {
  ReasonCode code;
  std::string_view name;
};

// Single source of truth for the public spelling of every reason code. The
// table is ordered by code; to_string() and reason_code_from_string() both
// consult it so the two can never disagree.
constexpr CodeName kCodeNames[] = {
    {ReasonCode::ok, "ok"},
    {ReasonCode::invalid_identifier, "invalid_identifier"},
    {ReasonCode::identifier_too_long, "identifier_too_long"},
    {ReasonCode::invalid_aspect_path, "invalid_aspect_path"},
    {ReasonCode::malformed_encoding, "malformed_encoding"},
    {ReasonCode::empty_input, "empty_input"},
    {ReasonCode::value_out_of_range, "value_out_of_range"},
    {ReasonCode::arithmetic_overflow, "arithmetic_overflow"},
    {ReasonCode::division_by_zero, "division_by_zero"},
    {ReasonCode::scale_mismatch, "scale_mismatch"},
    {ReasonCode::inexact_conversion, "inexact_conversion"},
    {ReasonCode::too_many_records, "too_many_records"},
    {ReasonCode::record_too_large, "record_too_large"},
    {ReasonCode::authority_unknown, "authority_unknown"},
    {ReasonCode::authority_boundary_violation, "authority_boundary_violation"},
    {ReasonCode::authority_role_not_observer, "authority_role_not_observer"},
    {ReasonCode::source_not_registered, "source_not_registered"},
    {ReasonCode::source_disappeared, "source_disappeared"},
    {ReasonCode::mutation_refused, "mutation_refused"},
    {ReasonCode::stale_authority_race, "stale_authority_race"},
    {ReasonCode::no_evidence, "no_evidence"},
    {ReasonCode::evidence_not_found, "evidence_not_found"},
    {ReasonCode::stale_epoch, "stale_epoch"},
    {ReasonCode::replayed_generation, "replayed_generation"},
    {ReasonCode::duplicate_evidence, "duplicate_evidence"},
    {ReasonCode::superseded_revision, "superseded_revision"},
    {ReasonCode::unsupported_aspect, "unsupported_aspect"},
    {ReasonCode::conflicting_evidence, "conflicting_evidence"},
    {ReasonCode::unit_incompatible, "unit_incompatible"},
    {ReasonCode::recovered_not_current, "recovered_not_current"},
    {ReasonCode::indeterminate, "indeterminate"},
    {ReasonCode::stale_evidence, "stale_evidence"},
    {ReasonCode::expired_evidence, "expired_evidence"},
    {ReasonCode::journal_missing, "journal_missing"},
    {ReasonCode::journal_version_unsupported, "journal_version_unsupported"},
    {ReasonCode::journal_header_corrupt, "journal_header_corrupt"},
    {ReasonCode::journal_interior_corruption, "journal_interior_corruption"},
    {ReasonCode::journal_torn_tail, "journal_torn_tail"},
    {ReasonCode::journal_locked, "journal_locked"},
    {ReasonCode::journal_io_error, "journal_io_error"},
    {ReasonCode::journal_already_open, "journal_already_open"},
    {ReasonCode::journal_not_open, "journal_not_open"},
    {ReasonCode::journal_path_invalid, "journal_path_invalid"},
    {ReasonCode::journal_commit_failed, "journal_commit_failed"},
    {ReasonCode::recovery_refused, "recovery_refused"},
    {ReasonCode::lock_unavailable, "lock_unavailable"},
    {ReasonCode::unknown_entity, "unknown_entity"},
    {ReasonCode::unknown_domain, "unknown_domain"},
    {ReasonCode::unsupported_query, "unsupported_query"},
    {ReasonCode::limit_exceeded, "limit_exceeded"},
    {ReasonCode::cancelled, "cancelled"},
    {ReasonCode::shutting_down, "shutting_down"},
    {ReasonCode::internal_error, "internal_error"},
    {ReasonCode::invalid_argument, "invalid_argument"},
    {ReasonCode::not_supported, "not_supported"},
    {ReasonCode::bucket_overflow, "bucket_overflow"},
};

}  // namespace

std::string_view to_string(ReasonCode code) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.code == code) {
      return entry.name;
    }
  }
  return std::string_view{"<unknown-reason>"};
}

bool reason_code_from_string(std::string_view text, ReasonCode& out) noexcept {
  for (const CodeName& entry : kCodeNames) {
    if (entry.name == text) {
      out = entry.code;
      return true;
    }
  }
  return false;
}

std::string to_debug_string(const Reason& reason) {
  std::string result(to_string(reason.code));
  if (!reason.detail.empty()) {
    result.push_back(':');
    result.push_back(' ');
    result.append(reason.detail);
  }
  return result;
}

}  // namespace fo
