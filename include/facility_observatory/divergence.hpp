// Facility Observatory - cross-domain divergence detection.
//
// Divergence is where two authoritative views of the same physical reality do
// not agree. The observatory reports the disagreement and the evidence behind
// both sides; it never decides which authority is right.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_DIVERGENCE_HPP
#define FACILITY_OBSERVATORY_DIVERGENCE_HPP

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/store.hpp"
#include "facility_observatory/strong.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

enum class DivergenceClass : std::uint8_t {
  source_disagreement = 0,     // two authorities assert different values
  aggregation_mismatch = 1,    // a published total disagrees with its constituents
  temporal_skew = 2,           // participants observed the world far apart in time
  coverage_gap = 3,            // a member of a set has published nothing
  unit_incompatible = 4,       // the same aspect is reported in unconvertible units
  identity_mismatch = 5,       // topology names a subject no authority has ever published
  recovered_versus_live = 6,   // durable history and live evidence disagree
};

FO_API std::string_view to_string(DivergenceClass divergence_class) noexcept;
FO_API Outcome<DivergenceClass> divergence_class_from_string(std::string_view text);

struct FO_API Divergence {
  DivergenceClass divergence_class{DivergenceClass::source_disagreement};
  EntityRef left_entity{};
  EntityRef right_entity{};
  AspectId aspect{};
  ObservationState left_state{ObservationState::unknown};
  ObservationState right_state{ObservationState::unknown};
  std::optional<Value> left_value{};
  std::optional<Value> right_value{};
  // right - left in the canonical unit, when both sides are comparable scalars.
  std::optional<FixedPoint> delta{};
  // |delta| / |left| at scale 6, when a relative measure is meaningful.
  std::optional<FixedPoint> relative_delta{};
  Unit unit{Unit::none};
  std::vector<EvidenceRef> left_evidence{};
  std::vector<EvidenceRef> right_evidence{};
  std::string detail{};

  friend bool operator==(const Divergence&, const Divergence&) = default;
};

// Canonical total order for deterministic output.
FO_API bool divergence_less(const Divergence& lhs, const Divergence& rhs) noexcept;

// ---------------------------------------------------------------------------
// Topology conventions
//
// Containment is itself evidence: a child publishes which parent it belongs to.
// That keeps membership an observation rather than an assumption, and lets a
// stale or disputed parent link be reported like any other evidence.
// ---------------------------------------------------------------------------
struct FO_API TopologyConventions {
  // Aspect on the child whose text value names the parent, e.g. "site:sea1".
  AspectId containment_aspect{};
  // Aspect on the parent holding a published scalar total.
  AspectId published_total_aspect{};
  // Aspect on each child holding the scalar contribution to that total.
  AspectId constituent_aspect{};

  static Outcome<TopologyConventions> make(std::string_view containment, std::string_view published_total,
                                           std::string_view constituent);

  friend bool operator==(const TopologyConventions&, const TopologyConventions&) = default;
};

struct FO_API DivergenceQuery {
  bool source_disagreement{true};
  bool aggregation_mismatch{true};
  bool temporal_skew{true};
  bool coverage_gap{true};
  bool unit_mismatch{true};
  bool recovered_versus_live{true};

  Duration temporal_skew_threshold{std::chrono::minutes(1)};
  // Relative tolerance at scale 6, for example 1000 means 0.1%.
  FixedPoint relative_tolerance{};

  std::size_t max_divergences{10000};

  static Outcome<DivergenceQuery> make_default();
};

class FO_API DivergenceDetector {
 public:
  DivergenceDetector(const EvidenceStore& store, TopologyConventions conventions, DivergenceQuery query);

  // Every divergence in the store, in canonical order.
  [[nodiscard]] Outcome<std::vector<Divergence>> detect(Timestamp at) const;

  // Divergences that involve this subject: its own aspects, and -- when it acts
  // as a parent under the topology conventions -- its constituents.
  [[nodiscard]] Outcome<std::vector<Divergence>> detect_for(const EntityRef& entity, Timestamp at) const;

 private:
  [[nodiscard]] Outcome<std::vector<EntityRef>> constituents_of(const EntityRef& parent, Timestamp at) const;
  void collect_subject(const EntityRef& entity, Timestamp at, std::vector<Divergence>& out) const;
  void collect_aggregation(const EntityRef& parent, Timestamp at, std::vector<Divergence>& out) const;

  const EvidenceStore& store_;
  TopologyConventions conventions_;
  DivergenceQuery query_;
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_DIVERGENCE_HPP
