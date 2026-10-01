// Facility Observatory - cross-domain divergence detection.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/divergence.hpp"

#include <algorithm>
#include <array>
#include <optional>
#include <string>
#include <utility>

namespace fo {
namespace {

constexpr int kRelativeScale = 6;

// right - left, with both sides brought into their shared canonical unit.
struct ScalarDelta {
  Unit unit{Unit::none};
  FixedPoint left{};
  FixedPoint right{};
  FixedPoint delta{};
  std::optional<FixedPoint> relative{};
};

// |magnitude| / |reference| as a plain fraction, so it is directly comparable
// with a tolerance expressed the same way (0.001 means one tenth of a percent).
std::optional<FixedPoint> relative_of(const FixedPoint& magnitude, const FixedPoint& reference) {
  if (reference.is_zero()) {
    return std::nullopt;
  }
  auto reference_abs = reference.absolute();
  if (!reference_abs) {
    return std::nullopt;
  }
  auto ratio = magnitude.reduced().divide(reference_abs.value().reduced().mantissa(), RoundingMode::half_even);
  if (!ratio) {
    return std::nullopt;
  }
  return ratio.value().reduced();
}

std::optional<ScalarDelta> scalar_delta(const Value& left, const Value& right) {
  if (!left.has_scalar() || !right.has_scalar() || !units_compatible(left.unit(), right.unit())) {
    return std::nullopt;
  }
  const Unit canonical = canonical_unit(left.unit());
  auto converted_left = convert(left.scalar_magnitude(), left.unit(), canonical);
  if (!converted_left) {
    return std::nullopt;
  }
  auto converted_right = convert(right.scalar_magnitude(), right.unit(), canonical);
  if (!converted_right) {
    return std::nullopt;
  }
  auto difference = converted_right.value().subtract(converted_left.value());
  if (!difference) {
    return std::nullopt;
  }
  ScalarDelta result;
  result.unit = canonical;
  result.left = converted_left.value().reduced();
  result.right = converted_right.value().reduced();
  result.delta = difference.value().reduced();
  auto magnitude = difference.value().absolute();
  if (magnitude) {
    result.relative = relative_of(magnitude.value(), converted_left.value().reduced());
  }
  return result;
}

bool exceeds_tolerance(const ScalarDelta& delta, const FixedPoint& tolerance) {
  if (delta.left.is_zero()) {
    return !delta.delta.is_zero();
  }
  if (!delta.relative.has_value()) {
    return false;
  }
  auto ordering = delta.relative.value().compare(tolerance);
  return ordering && ordering.value() == std::strong_ordering::greater;
}

}  // namespace

std::string_view to_string(DivergenceClass divergence_class) noexcept {
  switch (divergence_class) {
    case DivergenceClass::source_disagreement:
      return std::string_view{"source_disagreement"};
    case DivergenceClass::aggregation_mismatch:
      return std::string_view{"aggregation_mismatch"};
    case DivergenceClass::temporal_skew:
      return std::string_view{"temporal_skew"};
    case DivergenceClass::coverage_gap:
      return std::string_view{"coverage_gap"};
    case DivergenceClass::unit_incompatible:
      return std::string_view{"unit_incompatible"};
    case DivergenceClass::identity_mismatch:
      return std::string_view{"identity_mismatch"};
    case DivergenceClass::recovered_versus_live:
      return std::string_view{"recovered_versus_live"};
  }
  return std::string_view{"unknown"};
}

Outcome<DivergenceClass> divergence_class_from_string(std::string_view text) {
  const std::array<std::pair<std::string_view, DivergenceClass>, 7> table{{
      {"source_disagreement", DivergenceClass::source_disagreement},
      {"aggregation_mismatch", DivergenceClass::aggregation_mismatch},
      {"temporal_skew", DivergenceClass::temporal_skew},
      {"coverage_gap", DivergenceClass::coverage_gap},
      {"unit_incompatible", DivergenceClass::unit_incompatible},
      {"identity_mismatch", DivergenceClass::identity_mismatch},
      {"recovered_versus_live", DivergenceClass::recovered_versus_live},
  }};
  for (const auto& entry : table) {
    if (entry.first == text) {
      return Outcome<DivergenceClass>::ok(entry.second);
    }
  }
  return Outcome<DivergenceClass>::fail(ReasonCode::invalid_argument,
                                        "unknown divergence class '" + std::string(text) + "'");
}

bool divergence_less(const Divergence& lhs, const Divergence& rhs) noexcept {
  if (lhs.divergence_class != rhs.divergence_class) {
    return lhs.divergence_class < rhs.divergence_class;
  }
  if (lhs.left_entity != rhs.left_entity) {
    return lhs.left_entity < rhs.left_entity;
  }
  if (lhs.right_entity != rhs.right_entity) {
    return lhs.right_entity < rhs.right_entity;
  }
  if (lhs.aspect != rhs.aspect) {
    return lhs.aspect < rhs.aspect;
  }
  return lhs.detail < rhs.detail;
}

Outcome<TopologyConventions> TopologyConventions::make(std::string_view containment, std::string_view published_total,
                                                       std::string_view constituent) {
  auto containment_id = AspectId::parse(containment);
  if (!containment_id) {
    return containment_id.propagate<TopologyConventions>();
  }
  auto total_id = AspectId::parse(published_total);
  if (!total_id) {
    return total_id.propagate<TopologyConventions>();
  }
  auto constituent_id = AspectId::parse(constituent);
  if (!constituent_id) {
    return constituent_id.propagate<TopologyConventions>();
  }
  TopologyConventions conventions;
  conventions.containment_aspect = containment_id.value();
  conventions.published_total_aspect = total_id.value();
  conventions.constituent_aspect = constituent_id.value();
  return Outcome<TopologyConventions>::ok(conventions);
}

Outcome<DivergenceQuery> DivergenceQuery::make_default() {
  DivergenceQuery query;
  auto tolerance = FixedPoint::from_scaled(1000, kRelativeScale);  // 0.001 == 0.1%
  if (!tolerance) {
    return tolerance.propagate<DivergenceQuery>();
  }
  query.relative_tolerance = tolerance.value();
  return Outcome<DivergenceQuery>::ok(query);
}

DivergenceDetector::DivergenceDetector(const EvidenceStore& store, TopologyConventions conventions,
                                       DivergenceQuery query)
    : store_(store), conventions_(std::move(conventions)), query_(query) {}

Outcome<std::vector<EntityRef>> DivergenceDetector::constituents_of(const EntityRef& parent, Timestamp at) const {
  std::vector<EntityRef> result;
  const std::string wanted = parent.to_string();
  for (const EntityRef& subject : store_.subjects()) {
    if (subject == parent) {
      continue;
    }
    const Evaluation evaluation = store_.evaluate(subject, conventions_.containment_aspect, at);
    if (!evaluation.value.has_value() || evaluation.state == ObservationState::conflicting) {
      continue;
    }
    const Value& value = evaluation.value.value();
    if (value.kind() == ValueKind::text && value.text_body() == wanted) {
      result.push_back(subject);
    }
  }
  std::sort(result.begin(), result.end());
  return Outcome<std::vector<EntityRef>>::ok(std::move(result));
}

void DivergenceDetector::collect_subject(const EntityRef& entity, Timestamp at, std::vector<Divergence>& out) const {
  for (const AspectId& aspect : store_.aspects_of(entity)) {
    const Evaluation evaluation = store_.evaluate(entity, aspect, at);

    if (query_.unit_mismatch && evaluation.state == ObservationState::unsupported &&
        evaluation.code == ReasonCode::unit_incompatible) {
      Divergence divergence;
      divergence.divergence_class = DivergenceClass::unit_incompatible;
      divergence.left_entity = entity;
      divergence.right_entity = entity;
      divergence.aspect = aspect;
      divergence.left_state = evaluation.state;
      divergence.right_state = evaluation.state;
      divergence.left_evidence = evaluation.participants;
      divergence.detail = evaluation.detail;
      out.push_back(std::move(divergence));
    }

    if (query_.source_disagreement && evaluation.state == ObservationState::conflicting &&
        evaluation.disagreement.size() > 1) {
      const ConflictingValue& left = evaluation.disagreement.front();
      for (std::size_t index = 1; index < evaluation.disagreement.size(); ++index) {
        const ConflictingValue& right = evaluation.disagreement[index];
        Divergence divergence;
        divergence.divergence_class = DivergenceClass::source_disagreement;
        divergence.left_entity = entity;
        divergence.right_entity = entity;
        divergence.aspect = aspect;
        divergence.left_state = evaluation.state;
        divergence.right_state = evaluation.state;
        divergence.left_value = left.value;
        divergence.right_value = right.value;
        divergence.left_evidence.push_back(left.source);
        divergence.right_evidence.push_back(right.source);
        divergence.detail = "authority '" + left.source.key.authority.str() + "' reports " + left.value.to_string() +
                            " while authority '" + right.source.key.authority.str() + "' reports " +
                            right.value.to_string() + " for " + entity.to_string() + "." + aspect.str();
        auto difference = scalar_delta(left.value, right.value);
        if (difference.has_value()) {
          divergence.unit = difference->unit;
          divergence.delta = difference->delta;
          divergence.relative_delta = difference->relative;
        }
        out.push_back(std::move(divergence));
      }
    }

    if (query_.temporal_skew) {
      const EvidenceRef* earliest_ref = nullptr;
      const EvidenceRef* latest_ref = nullptr;
      for (const EvidenceRef& reference : evaluation.participants) {
        if (!reference.observed_at.is_set()) {
          continue;
        }
        if (earliest_ref == nullptr || reference.observed_at < earliest_ref->observed_at) {
          earliest_ref = &reference;
        }
        if (latest_ref == nullptr || latest_ref->observed_at > latest_ref->observed_at) {
          latest_ref = &reference;
        }
      }
      if (earliest_ref != nullptr && latest_ref != nullptr && earliest_ref != latest_ref) {
        auto spread = duration_between(latest_ref->observed_at, earliest_ref->observed_at);
        if (spread && spread.value() > query_.temporal_skew_threshold) {
          Divergence divergence;
          divergence.divergence_class = DivergenceClass::temporal_skew;
          divergence.left_entity = entity;
          divergence.right_entity = entity;
          divergence.aspect = aspect;
          divergence.left_evidence.push_back(*earliest_ref);
          divergence.right_evidence.push_back(*latest_ref);
          divergence.detail = "participants observed " + entity.to_string() + "." + aspect.str() + " " +
                              to_string(spread.value()) + " apart, beyond the threshold of " +
                              to_string(query_.temporal_skew_threshold);
          out.push_back(std::move(divergence));
        }
      }
    }

    if (query_.recovered_versus_live) {
      const std::vector<EvidenceRecord> history = store_.history(entity, aspect);
      const EvidenceRecord* recovered_record = nullptr;
      const EvidenceRecord* live_record = nullptr;
      for (const EvidenceRecord& record : history) {
        if (record.provenance.durability == Durability::recovered) {
          if (recovered_record == nullptr) {
            recovered_record = &record;
          }
        } else if (live_record == nullptr) {
          live_record = &record;
        }
      }
      if (recovered_record != nullptr && live_record != nullptr &&
          !value_equivalent(recovered_record->value, live_record->value)) {
        Divergence divergence;
        divergence.divergence_class = DivergenceClass::recovered_versus_live;
        divergence.left_entity = entity;
        divergence.right_entity = entity;
        divergence.aspect = aspect;
        divergence.left_value = recovered_record->value;
        divergence.right_value = live_record->value;
        divergence.left_evidence.push_back(to_reference(*recovered_record));
        divergence.right_evidence.push_back(to_reference(*live_record));
        divergence.detail = "durable history holds " + recovered_record->value.to_string() +
                            " while live evidence reports " + live_record->value.to_string() + " for " +
                            entity.to_string() + "." + aspect.str();
        out.push_back(std::move(divergence));
      }
    }
  }
}

void DivergenceDetector::collect_aggregation(const EntityRef& parent, Timestamp at,
                                             std::vector<Divergence>& out) const {
  auto constituents = constituents_of(parent, at);
  if (!constituents) {
    return;
  }
  const std::vector<EntityRef>& children = constituents.value();

  if (query_.coverage_gap) {
    for (const EntityRef& child : children) {
      bool publishes_constituent = false;
      for (const AspectId& aspect : store_.aspects_of(child)) {
        if (aspect == conventions_.constituent_aspect) {
          publishes_constituent = true;
          break;
        }
      }
      if (publishes_constituent) {
        continue;
      }
      Divergence divergence;
      divergence.divergence_class = DivergenceClass::coverage_gap;
      divergence.left_entity = parent;
      divergence.right_entity = child;
      divergence.aspect = conventions_.constituent_aspect;
      divergence.detail = "topology places " + child.to_string() + " under " + parent.to_string() +
                          " but no authority has published aspect '" + conventions_.constituent_aspect.str() +
                          "' for it, so any total for the parent is incomplete";
      out.push_back(std::move(divergence));
    }
  }

  if (!query_.aggregation_mismatch || children.empty()) {
    return;
  }

  const Evaluation published = store_.evaluate(parent, conventions_.published_total_aspect, at);
  if (!published.value.has_value() || !published.value.value().has_scalar()) {
    return;
  }
  const AggregateResult computed =
      store_.aggregate(parent, children, conventions_.constituent_aspect, Aggregation::sum, false, at);
  if (!computed.value.has_value() || !computed.value.value().has_scalar()) {
    return;
  }
  const Value& published_value = published.value.value();
  const Value& constituent_value = computed.value.value();

  if (!units_compatible(published_value.unit(), constituent_value.unit())) {
    Divergence divergence;
    divergence.divergence_class = DivergenceClass::unit_incompatible;
    divergence.left_entity = parent;
    divergence.right_entity = parent;
    divergence.aspect = conventions_.published_total_aspect;
    divergence.left_value = published_value;
    divergence.right_value = constituent_value;
    divergence.left_evidence = published.participants;
    divergence.right_evidence = computed.contributors;
    divergence.detail = "published total is in " + std::string(to_string(published_value.unit())) +
                        " while constituent evidence is in " +
                        std::string(to_string(constituent_value.unit()));
    out.push_back(std::move(divergence));
    return;
  }

  auto difference = scalar_delta(published_value, constituent_value);
  if (!difference.has_value()) {
    return;
  }
  if (!exceeds_tolerance(difference.value(), query_.relative_tolerance)) {
    return;
  }

  Divergence divergence;
  divergence.divergence_class = DivergenceClass::aggregation_mismatch;
  divergence.left_entity = parent;
  divergence.right_entity = parent;
  divergence.aspect = conventions_.published_total_aspect;
  divergence.left_state = published.state;
  divergence.right_state = computed.state;
  divergence.left_value = published_value;
  divergence.right_value = constituent_value;
  divergence.delta = difference->delta;
  divergence.relative_delta = difference->relative;
  divergence.unit = difference->unit;
  divergence.left_evidence = published.participants;
  divergence.right_evidence = computed.contributors;
  divergence.detail = "published " + parent.to_string() + "." + conventions_.published_total_aspect.str() + " is " +
                      published_value.to_string() + " but the " + std::to_string(children.size()) +
                      " constituent values sum to " + constituent_value.to_string() + " (delta " +
                      difference->delta.to_string() + " " + std::string(to_string(difference->unit)) + ")";
  out.push_back(std::move(divergence));
}

Outcome<std::vector<Divergence>> DivergenceDetector::detect(Timestamp at) const {
  std::vector<Divergence> found;
  if (!at.is_set()) {
    return Outcome<std::vector<Divergence>>::fail(ReasonCode::indeterminate,
                                                  "divergence detection requires a set evaluation instant");
  }
  for (const EntityRef& entity : store_.subjects()) {
    collect_subject(entity, at, found);
    collect_aggregation(entity, at, found);
    if (found.size() > query_.max_divergences) {
      return Outcome<std::vector<Divergence>>::fail(
          ReasonCode::limit_exceeded,
          "divergence detection exceeded its bound of " + std::to_string(query_.max_divergences) + " results");
    }
  }
  std::sort(found.begin(), found.end(), divergence_less);
  found.erase(std::unique(found.begin(), found.end()), found.end());
  return Outcome<std::vector<Divergence>>::ok(std::move(found));
}

Outcome<std::vector<Divergence>> DivergenceDetector::detect_for(const EntityRef& entity, Timestamp at) const {
  std::vector<Divergence> found;
  if (!at.is_set()) {
    return Outcome<std::vector<Divergence>>::fail(ReasonCode::indeterminate,
                                                  "divergence detection requires a set evaluation instant");
  }
  collect_subject(entity, at, found);
  collect_aggregation(entity, at, found);

  auto constituents = constituents_of(entity, at);
  if (constituents) {
    for (const EntityRef& child : constituents.value()) {
      collect_subject(child, at, found);
    }
  }

  // A subject also takes part in the aggregation of every parent that claims it,
  // so a change to a constituent is visible from the constituent's own view.
  for (const EntityRef& candidate : store_.subjects()) {
    if (candidate == entity) {
      continue;
    }
    auto children = constituents_of(candidate, at);
    if (!children) {
      continue;
    }
    const std::vector<EntityRef>& list = children.value();
    if (std::find(list.begin(), list.end(), entity) != list.end()) {
      collect_aggregation(candidate, at, found);
    }
    if (found.size() > query_.max_divergences) {
      return Outcome<std::vector<Divergence>>::fail(
          ReasonCode::limit_exceeded,
          "divergence detection exceeded its bound of " + std::to_string(query_.max_divergences) + " results");
    }
  }

  std::sort(found.begin(), found.end(), divergence_less);
  found.erase(std::unique(found.begin(), found.end()), found.end());
  return Outcome<std::vector<Divergence>>::ok(std::move(found));
}

}  // namespace fo
