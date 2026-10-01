// Facility Observatory - authority identity and boundary contracts.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/authority.hpp"

#include <array>

namespace fo {
namespace {

struct KindName {
  AuthorityKind kind;
  std::string_view name;
};

constexpr std::array<KindName, 9> kKindNames{{
    {AuthorityKind::dccp, "dccp"},
    {AuthorityKind::asi, "asi"},
    {AuthorityKind::dfi, "dfi"},
    {AuthorityKind::bms, "bms"},
    {AuthorityKind::dcim, "dcim"},
    {AuthorityKind::plant, "plant"},
    {AuthorityKind::economic, "economic"},
    {AuthorityKind::synthetic, "synthetic"},
    {AuthorityKind::unknown, "unknown"},
}};

}  // namespace

std::string_view to_string(AuthorityKind kind) noexcept {
  for (const KindName& entry : kKindNames) {
    if (entry.kind == kind) {
      return entry.name;
    }
  }
  return std::string_view{"unknown"};
}

Outcome<AuthorityKind> authority_kind_from_string(std::string_view text) {
  for (const KindName& entry : kKindNames) {
    if (entry.name == text) {
      return Outcome<AuthorityKind>::ok(entry.kind);
    }
  }
  return Outcome<AuthorityKind>::fail(ReasonCode::invalid_argument,
                                      "unknown authority kind '" + std::string(text) + "'");
}

std::string_view to_string(AuthorityRole role) noexcept {
  switch (role) {
    case AuthorityRole::authority:
      return std::string_view{"authority"};
    case AuthorityRole::advisory:
      return std::string_view{"advisory"};
    case AuthorityRole::synthetic:
      return std::string_view{"synthetic"};
    case AuthorityRole::unknown:
      return std::string_view{"unknown"};
  }
  return std::string_view{"unknown"};
}

Outcome<AuthorityRole> authority_role_from_string(std::string_view text) {
  if (text == "authority") {
    return Outcome<AuthorityRole>::ok(AuthorityRole::authority);
  }
  if (text == "advisory") {
    return Outcome<AuthorityRole>::ok(AuthorityRole::advisory);
  }
  if (text == "synthetic") {
    return Outcome<AuthorityRole>::ok(AuthorityRole::synthetic);
  }
  if (text == "unknown") {
    return Outcome<AuthorityRole>::ok(AuthorityRole::unknown);
  }
  return Outcome<AuthorityRole>::fail(ReasonCode::invalid_argument,
                                      "unknown authority role '" + std::string(text) + "'");
}

std::string_view to_string(BoundaryOperation operation) noexcept {
  switch (operation) {
    case BoundaryOperation::read_evidence:
      return std::string_view{"read_evidence"};
    case BoundaryOperation::record_evidence:
      return std::string_view{"record_evidence"};
    case BoundaryOperation::evaluate_view:
      return std::string_view{"evaluate_view"};
    case BoundaryOperation::inspect_history:
      return std::string_view{"inspect_history"};
    case BoundaryOperation::command_facility_state:
      return std::string_view{"command_facility_state"};
    case BoundaryOperation::set_policy:
      return std::string_view{"set_policy"};
    case BoundaryOperation::transition_incident:
      return std::string_view{"transition_incident"};
    case BoundaryOperation::allocate_capacity:
      return std::string_view{"allocate_capacity"};
    case BoundaryOperation::place_workload:
      return std::string_view{"place_workload"};
    case BoundaryOperation::schedule_maintenance:
      return std::string_view{"schedule_maintenance"};
    case BoundaryOperation::control_power:
      return std::string_view{"control_power"};
    case BoundaryOperation::control_cooling:
      return std::string_view{"control_cooling"};
    case BoundaryOperation::drive_recovery:
      return std::string_view{"drive_recovery"};
    case BoundaryOperation::mutate_identity:
      return std::string_view{"mutate_identity"};
    case BoundaryOperation::publish_financial_state:
      return std::string_view{"publish_financial_state"};
  }
  return std::string_view{"unknown_operation"};
}

bool is_observer_operation(BoundaryOperation operation) noexcept {
  switch (operation) {
    case BoundaryOperation::read_evidence:
    case BoundaryOperation::record_evidence:
    case BoundaryOperation::evaluate_view:
    case BoundaryOperation::inspect_history:
      return true;
    default:
      return false;
  }
}

Status assert_observer_operation(BoundaryOperation operation) {
  if (is_observer_operation(operation)) {
    return success();
  }
  return Status::fail(ReasonCode::mutation_refused,
                      "operation '" + std::string(to_string(operation)) +
                          "' belongs to an adjacent authority and is not owned by the observatory");
}

std::string_view systems_boundary_statement() noexcept {
  return std::string_view{
      "Facility Observatory owns observation, normalization, correlation, explanation, provenance, freshness, "
      "divergence detection and historical inspection of facility-wide authoritative evidence. It does not own or "
      "mutate facility state, incident lifecycle, capacity, placement, policy, power, cooling, maintenance, "
      "recovery, or the authority of adjacent DCCP, ASI, DFI, BMS and DCIM runtimes."};
}

}  // namespace fo
