// Facility Observatory - authority identity and boundary contracts.
//
// The observatory records what authorities publish. It never becomes an
// authority: seeing another runtime's evidence does not transfer its authority.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_AUTHORITY_HPP
#define FACILITY_OBSERVATORY_AUTHORITY_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/export.hpp"
#include "facility_observatory/strong.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

// Which runtime family published the evidence. Adjacent DCCP, ASI and DFI
// authorities are named explicitly so that provenance is never inferred from
// the shape of a payload.
enum class AuthorityKind : std::uint8_t {
  dccp = 0,       // Data Center Control Plane
  asi = 1,        // Asset / System Identity
  dfi = 2,        // Dependency & Facility Intelligence
  bms = 3,        // Building Management System
  dcim = 4,       // Data Center Infrastructure Management
  plant = 5,      // plant / telemetry acquisition
  economic = 6,   // economic or financial feed
  synthetic = 7,  // modelled evidence with no physical counterpart
  unknown = 255,
};

enum class AuthorityRole : std::uint8_t {
  authority = 0,  // publishes authoritative state for its own domain
  advisory = 1,   // publishes advisory or derived evidence only
  synthetic = 2,  // publishes modelled evidence for a subject with no real source
  unknown = 255,
};

FO_API std::string_view to_string(AuthorityKind kind) noexcept;
FO_API Outcome<AuthorityKind> authority_kind_from_string(std::string_view text);
FO_API std::string_view to_string(AuthorityRole role) noexcept;
FO_API Outcome<AuthorityRole> authority_role_from_string(std::string_view text);

// A registered publisher.
struct FO_API AuthorityDescriptor {
  AuthorityId id{};
  AuthorityKind kind{AuthorityKind::unknown};
  AuthorityRole role{AuthorityRole::unknown};
  Token label{};
  // Highest epoch this authority has published. Evidence carrying a lower epoch
  // is historical: it is retained, but it can never become current again.
  Epoch epoch{};
  Timestamp epoch_published_at{};
  // True when every record from this authority is modelled rather than measured.
  bool synthetic{false};

  friend bool operator==(const AuthorityDescriptor&, const AuthorityDescriptor&) = default;
};

// ---------------------------------------------------------------------------
// Authority boundary
//
// Every operation this runtime could be asked to perform is enumerated, and the
// ones that would transfer authority from an adjacent runtime to the observatory
// are refused with an explicit reason rather than partially attempted.
// ---------------------------------------------------------------------------
enum class BoundaryOperation : std::uint8_t {
  read_evidence = 0,
  record_evidence = 1,
  evaluate_view = 2,
  inspect_history = 3,
  // Everything below belongs to an authority the observatory does not own.
  command_facility_state = 16,
  set_policy = 17,
  transition_incident = 18,
  allocate_capacity = 19,
  place_workload = 20,
  schedule_maintenance = 21,
  control_power = 22,
  control_cooling = 23,
  drive_recovery = 24,
  mutate_identity = 25,
  publish_financial_state = 26,
};

FO_API std::string_view to_string(BoundaryOperation operation) noexcept;
FO_API bool is_observer_operation(BoundaryOperation operation) noexcept;

// Succeeds for observer operations, refuses everything else with
// mutation_refused and a detail naming the operation.
FO_API Status assert_observer_operation(BoundaryOperation operation);

// Human-readable statement of what this runtime does and does not own.
FO_API std::string_view systems_boundary_statement() noexcept;

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_AUTHORITY_HPP
