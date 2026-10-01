// Facility Observatory - evidence store evaluation tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "facility_observatory/store.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

void register_pair(EvidenceStore& store) {
  FO_REQUIRE(store.register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  FO_REQUIRE(store.register_authority(make_authority("bms-a", AuthorityKind::bms)).has_value());
}

}  // namespace

FO_TEST(store_authority_registry) {
  EvidenceStore store;
  const AuthorityDescriptor descriptor = make_authority("dccp-a", AuthorityKind::dccp, 4);
  FO_REQUIRE(store.register_authority(descriptor).has_value());
  FO_REQUIRE(store.knows_authority(descriptor.id));
  FO_REQUIRE_EQ(store.authority_count(), 1);

  FO_REQUIRE(store.register_authority(descriptor).has_value());
  FO_REQUIRE_EQ(store.authority_count(), 1);

  AuthorityDescriptor renamed = descriptor;
  renamed.kind = AuthorityKind::asi;
  auto refused = store.register_authority(renamed);
  FO_REQUIRE(!refused.has_value());
  FO_REQUIRE(refused.code() == ReasonCode::authority_boundary_violation);

  AuthorityDescriptor regressed = descriptor;
  regressed.epoch = Epoch::from_value(1);
  auto stale = store.register_authority(regressed);
  FO_REQUIRE(!stale.has_value());
  FO_REQUIRE(stale.code() == ReasonCode::stale_epoch);

  FO_REQUIRE(!store.register_authority(AuthorityDescriptor{}).has_value());
  FO_REQUIRE(!store.authority(AuthorityId::parse("missing").value()).has_value());
}

FO_TEST(store_admission_paths) {
  EvidenceStore store;
  register_pair(store);

  const EvidenceRecord first =
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 5, 1, "4.25", "kilowatts", 100);
  const AdmissionOutcome admitted = store.admit(first);
  FO_REQUIRE(admitted.code == ReasonCode::ok);
  FO_REQUIRE(admitted.admitted);
  FO_REQUIRE(admitted.retained);
  FO_REQUIRE(admitted.fence_advanced);
  FO_REQUIRE_EQ(store.record_count(), 1);

  const AdmissionOutcome duplicate = store.admit(first);
  FO_REQUIRE(duplicate.code == ReasonCode::duplicate_evidence);
  FO_REQUIRE(duplicate.duplicate);
  FO_REQUIRE(!duplicate.retained);
  FO_REQUIRE_EQ(store.record_count(), 1);

  EvidenceRecord older = first;
  older.generation = Generation::from_value(4);
  const AdmissionOutcome superseded = store.admit(older);
  FO_REQUIRE(superseded.code == ReasonCode::superseded_revision);
  FO_REQUIRE(superseded.retained);
  FO_REQUIRE(!superseded.admitted);
  FO_REQUIRE_EQ(store.record_count(), 2);

  EvidenceRecord same_position = first;
  same_position.value = Value::make_scalar(FixedPoint::parse("9.99").value(), Unit::kilowatts).value();
  const AdmissionOutcome contradiction = store.admit(same_position);
  FO_REQUIRE(contradiction.code == ReasonCode::conflicting_evidence);
  FO_REQUIRE(contradiction.retained);

  const EvidenceRecord unregistered =
      make_evidence("dcim-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  const AdmissionOutcome unknown = store.admit(unregistered);
  FO_REQUIRE(unknown.code == ReasonCode::authority_unknown);
  FO_REQUIRE(!unknown.retained);
}

FO_TEST(store_admission_shape_refusals) {
  EvidenceStore store;
  register_pair(store);

  EvidenceRecord record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  record.epoch = Epoch{};
  FO_REQUIRE(store.admit(record).code == ReasonCode::invalid_argument);

  record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  record.generation = Generation{};
  FO_REQUIRE(store.admit(record).code == ReasonCode::invalid_argument);

  record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  record.observed_at = Timestamp{};
  FO_REQUIRE(store.admit(record).code == ReasonCode::indeterminate);

  record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  record.observed_at = at_seconds(10);
  record.published_at = at_seconds(5);
  FO_REQUIRE(store.admit(record).code == ReasonCode::indeterminate);

  record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
  record.explanation.assign(4096, 'x');
  FO_REQUIRE(store.admit(record).code == ReasonCode::record_too_large);
}

FO_TEST(store_epoch_fencing) {
  EvidenceStore store;
  register_pair(store);

  const EvidenceRecord first =
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "4.25", "kilowatts", 100);
  FO_REQUIRE(store.admit(first).code == ReasonCode::ok);

  EvidenceRecord newer_epoch = first;
  newer_epoch.epoch = Epoch::from_value(2);
  newer_epoch.observed_at = at_seconds(200);
  newer_epoch.published_at = at_seconds(200);
  const AdmissionOutcome advanced = store.admit(newer_epoch);
  FO_REQUIRE(advanced.code == ReasonCode::ok);
  FO_REQUIRE(advanced.fence_advanced);
  FO_REQUIRE_EQ(store.authority(first.key.authority).value().epoch.value(), 2);

  EvidenceRecord historical = first;
  historical.generation = Generation::from_value(2);
  historical.observed_at = at_seconds(150);
  historical.published_at = at_seconds(150);
  const AdmissionOutcome fenced = store.admit(historical);
  FO_REQUIRE(fenced.code == ReasonCode::stale_epoch);
  FO_REQUIRE(fenced.retained);
  FO_REQUIRE(!fenced.admitted);

  const Evaluation evaluation = store.evaluate(first.key.entity, first.key.aspect, at_seconds(200));
  FO_REQUIRE(evaluation.state == ObservationState::known);
  FO_REQUIRE_EQ(evaluation.contributors.size(), 1);
  FO_REQUIRE_EQ(evaluation.contributors.front().epoch.value(), 2);
}

FO_TEST(store_explicit_epoch_advance) {
  EvidenceStore store;
  register_pair(store);
  const AuthorityId dccp = AuthorityId::parse("dccp-a").value();

  const AdmissionOutcome replayed = store.advance_epoch(dccp, Epoch::from_value(1), at_seconds(10));
  FO_REQUIRE(replayed.code == ReasonCode::replayed_generation);
  FO_REQUIRE(store.advance_epoch(dccp, Epoch::from_value(2), at_seconds(10)).code == ReasonCode::ok);
  FO_REQUIRE(store.advance_epoch(dccp, Epoch::from_value(2), at_seconds(10)).code ==
             ReasonCode::replayed_generation);
  FO_REQUIRE(store.advance_epoch(AuthorityId::parse("missing").value(), Epoch::from_value(2), at_seconds(10)).code ==
             ReasonCode::authority_unknown);
  FO_REQUIRE(store.advance_epoch(dccp, Epoch{}, at_seconds(10)).code == ReasonCode::invalid_argument);
}

FO_TEST(store_evaluation_states) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  const Evaluation missing = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(missing.state == ObservationState::unknown);
  FO_REQUIRE(missing.code == ReasonCode::no_evidence);
  FO_REQUIRE(!missing.value.has_value());

  FO_REQUIRE(store
                 .admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "4.25", "kilowatts", 100))
                 .retained);
  const Evaluation known = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(known.state == ObservationState::known);
  FO_REQUIRE(known.value.has_value());
  FO_REQUIRE_EQ(known.value.value().to_string(), std::string("4250 watts"));
  FO_REQUIRE_EQ(known.participants.size(), 1);

  // Default policy: fresh for five minutes, stale for thirty. Ten minutes after
  // the observation the value is still reported, but flagged stale.
  const Evaluation stale = store.evaluate(rack, draw, at_seconds(700));
  FO_REQUIRE(stale.state == ObservationState::stale);
  FO_REQUIRE(stale.value.has_value());
  FO_REQUIRE(stale.code == ReasonCode::stale_evidence);

  FO_REQUIRE(store
                 .admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "4.90", "kilowatts", 100))
                 .retained);
  const Evaluation conflicting = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(conflicting.state == ObservationState::conflicting);
  FO_REQUIRE_EQ(conflicting.disagreement.size(), 2);
  FO_REQUIRE(!conflicting.value.has_value());
  FO_REQUIRE(conflicting.detail.find("preserved") != std::string::npos);
}

FO_TEST(store_unit_normalisation_and_incompatibility) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1.5", "kilowatts", 100))
                  .retained);
  const Evaluation equivalent = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(equivalent.state == ObservationState::known);
  FO_REQUIRE_EQ(equivalent.value.value().to_string(), std::string("1500 watts"));
  FO_REQUIRE(equivalent.value.value().unit() == Unit::watts);

  EvidenceStore other;
  register_pair(other);
  FO_REQUIRE(other.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "10", "watts", 100))
                  .retained);
  FO_REQUIRE(other.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "25", "celsius", 100))
                  .retained);
  const Evaluation unsupported = other.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(unsupported.state == ObservationState::unsupported);
  FO_REQUIRE(unsupported.code == ReasonCode::unit_incompatible);
}

FO_TEST(store_future_dated_evidence_is_indeterminate) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 5000))
                  .retained);
  const Evaluation evaluation = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(evaluation.state == ObservationState::indeterminate);
  FO_REQUIRE(evaluation.code == ReasonCode::indeterminate);
}

FO_TEST(store_recovered_evidence_semantics) {
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  {
    EvidenceStore store;
    register_pair(store);
    EvidenceRecord record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100);
    record.provenance.durability = Durability::recovered;
    FO_REQUIRE(store.admit(record).retained);
    const Evaluation evaluation = store.evaluate(rack, draw, at_seconds(100));
    FO_REQUIRE(evaluation.state == ObservationState::stale);
    FO_REQUIRE(evaluation.code == ReasonCode::recovered_not_current);
  }

  {
    EvidenceStore store;
    register_pair(store);
    const AspectPolicy policy = make_policy("topology.parent", 60, 600, AspectClass::static_identity);
    FO_REQUIRE(store.policies().set(policy).has_value());
    EvidenceRecord record = make_text_evidence("dccp-a", "rack:r1", "topology.parent", 1, "site:sea1", 100);
    record.provenance.durability = Durability::recovered;
    FO_REQUIRE(store.admit(record).retained);
    const Evaluation evaluation = store.evaluate(rack, AspectId::parse("topology.parent").value(), at_seconds(100));
    FO_REQUIRE(evaluation.state == ObservationState::known);
  }
}

FO_TEST(store_precedence_resolves_without_hiding_disagreement) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  FO_REQUIRE(store
                 .admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "4.25", "kilowatts", 100))
                 .retained);
  FO_REQUIRE(store
                 .admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "4.90", "kilowatts", 100))
                 .retained);

  AspectPolicy policy = make_policy("power.draw", 60, 600);
  policy.precedence.push_back(AuthorityId::parse("dccp-a").value());
  FO_REQUIRE(store.policies().set(policy).has_value());

  const Evaluation resolved = store.evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(resolved.state == ObservationState::known);
  FO_REQUIRE(resolved.code == ReasonCode::ok);
  FO_REQUIRE(resolved.value.has_value());
  FO_REQUIRE_EQ(resolved.value.value().to_string(), std::string("4250 watts"));
  FO_REQUIRE_EQ(resolved.disagreement.size(), 2);

  EvidenceStore other;
  register_pair(other);
  FO_REQUIRE(other.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100))
                  .retained);
  FO_REQUIRE(other.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "2", "watts", 100))
                  .retained);
  AspectPolicy irrelevant = make_policy("power.draw", 60, 600);
  irrelevant.precedence.push_back(AuthorityId::parse("dcim-x").value());
  FO_REQUIRE(other.policies().set(irrelevant).has_value());
  FO_REQUIRE(other.evaluate(rack, draw, at_seconds(100)).state == ObservationState::conflicting);
}

FO_TEST(store_history_is_canonical) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "2", "watts", 300))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 3, 1, "3", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 4, 1, "4", "watts", 200))
                  .retained);

  const std::vector<EvidenceRecord> history = store.history(rack, draw);
  FO_REQUIRE_EQ(history.size(), 3);
  for (std::size_t index = 1; index < history.size(); ++index) {
    FO_REQUIRE(canonical_less(history[index - 1], history[index]));
  }
  FO_REQUIRE_EQ(history.front().key.authority.str(), std::string("bms-a"));
}

FO_TEST(store_permutation_invariance) {
  Rng rng(0xA11CEULL);
  std::vector<EvidenceRecord> base;
  base.push_back(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 5, 1, "1.5", "kilowatts", 100));
  base.push_back(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 4, 1, "1.4", "kilowatts", 90));
  base.push_back(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 2, 1, "1600", "watts", 100));
  base.push_back(make_evidence("dccp-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "2", "kilowatts", 100));
  base.push_back(make_text_evidence("dccp-a", "rack:r1", "topology.parent", 1, "site:sea1", 100));
  base.push_back(make_text_evidence("dccp-a", "rack:r2", "topology.parent", 1, "site:sea1", 100));
  base.push_back(make_evidence("dccp-a", "telemetry", "site:sea1", "power.total", 1, 1, 1, "3.6", "kilowatts", 100));

  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  std::string reference_digest;
  std::string reference_evaluation;
  std::string reference_history;

  for (int round = 0; round < 40; ++round) {
    std::vector<EvidenceRecord> shuffled = base;
    for (std::size_t index = shuffled.size(); index > 1; --index) {
      const std::size_t swap = static_cast<std::size_t>(rng.below(index));
      std::swap(shuffled[index - 1], shuffled[swap]);
    }
    EvidenceStore store;
    register_pair(store);
    for (const EvidenceRecord& record : shuffled) {
      FO_REQUIRE(store.admit(record).retained);
    }
    const std::string digest = store.digest();
    const Evaluation evaluation = store.evaluate(rack, draw, at_seconds(100));
    std::string rendered = std::string(to_string(evaluation.state)) + "|" + evaluation.detail;
    if (evaluation.value.has_value()) {
      rendered.append("|");
      rendered.append(evaluation.value.value().to_string());
    }
    std::string history;
    for (const EvidenceRecord& record : store.history(rack, draw)) {
      history.append(record.key.authority.str());
      history.push_back(':');
      history.append(std::to_string(record.generation.value()));
      history.push_back(';');
    }

    if (round == 0) {
      reference_digest = digest;
      reference_evaluation = rendered;
      reference_history = history;
      continue;
    }
    FO_REQUIRE_MESSAGE(digest == reference_digest,
                       "digest changed with admission order on round " + std::to_string(round));
    FO_REQUIRE_MESSAGE(rendered == reference_evaluation,
                       "evaluation changed with admission order: " + rendered + " vs " + reference_evaluation);
    FO_REQUIRE_MESSAGE(history == reference_history, "history ordering changed with admission order");
  }
}

FO_TEST(store_aggregation_kinds) {
  EvidenceStore store;
  register_pair(store);
  std::vector<EntityRef> racks;
  for (int index = 1; index <= 3; ++index) {
    const std::string subject = "rack:r" + std::to_string(index);
    racks.push_back(EntityRef::parse(subject).value());
    const std::string load = std::to_string(index);
    FO_REQUIRE(store
                   .admit(make_evidence("dccp-a", "telemetry", subject.c_str(), "power.draw", 1, 1, 1, load.c_str(),
                                        "kilowatts", 100))
                   .retained);
  }
  const AspectId draw = AspectId::parse("power.draw").value();
  const EntityRef site = EntityRef::parse("site:sea1").value();

  const AggregateResult sum = store.aggregate(site, racks, draw, Aggregation::sum, true, at_seconds(100));
  FO_REQUIRE(sum.state == ObservationState::known);
  FO_REQUIRE_EQ(sum.value.value().to_string(), std::string("6000 watts"));
  FO_REQUIRE_EQ(sum.observed_contributors, 3);

    FO_REQUIRE_EQ(
      store.aggregate(site, racks, draw, Aggregation::minimum, true, at_seconds(100)).value.value().to_string(),
      std::string("1000 watts"));
  FO_REQUIRE_EQ(
      store.aggregate(site, racks, draw, Aggregation::maximum, true, at_seconds(100)).value.value().to_string(),
      std::string("3000 watts"));
  FO_REQUIRE_EQ(store.aggregate(site, racks, draw, Aggregation::count, true, at_seconds(100)).value.value().to_string(),
                std::string("3 count"));
  FO_REQUIRE_EQ(store.aggregate(site, racks, draw, Aggregation::mean, true, at_seconds(100)).value.value().to_string(),
                std::string("2000 watts"));
}

FO_TEST(store_aggregation_completeness_and_units) {
  EvidenceStore store;
  register_pair(store);
  const AspectId draw = AspectId::parse("power.draw").value();
  const EntityRef site = EntityRef::parse("site:sea1").value();

  std::vector<EntityRef> racks;
  racks.push_back(EntityRef::parse("rack:r1").value());
  racks.push_back(EntityRef::parse("rack:r2").value());
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1000", "watts", 100))
                  .retained);

  const AggregateResult incomplete = store.aggregate(site, racks, draw, Aggregation::sum, true, at_seconds(100));
  FO_REQUIRE(incomplete.state == ObservationState::unknown);
  FO_REQUIRE(!incomplete.value.has_value());
  FO_REQUIRE_EQ(incomplete.missing.size(), 1);
  FO_REQUIRE(incomplete.detail.find("complete total cannot be produced") != std::string::npos);

  const AggregateResult partial = store.aggregate(site, racks, draw, Aggregation::sum, false, at_seconds(100));
  FO_REQUIRE(partial.state == ObservationState::known);
  FO_REQUIRE_EQ(partial.value.value().to_string(), std::string("1000 watts"));

  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "1", "kilowatts", 100))
                  .retained);
  const AggregateResult mixed = store.aggregate(site, racks, draw, Aggregation::sum, true, at_seconds(100));
  FO_REQUIRE(mixed.state == ObservationState::known);
  FO_REQUIRE_EQ(mixed.value.value().to_string(), std::string("2000 watts"));
}

FO_TEST(store_aggregation_refuses_non_scalar) {
  EvidenceStore store;
  register_pair(store);
  const EntityRef site = EntityRef::parse("site:sea1").value();
  const std::vector<EntityRef> racks{EntityRef::parse("rack:r1").value()};

  FO_REQUIRE(store.admit(make_text_evidence("dccp-a", "rack:r1", "topology.parent", 1, "site:sea1", 100)).retained);
  const AggregateResult textual = store.aggregate(site, racks, AspectId::parse("topology.parent").value(),
                                                  Aggregation::sum, true, at_seconds(100));
  FO_REQUIRE(textual.state == ObservationState::unsupported);
  FO_REQUIRE(textual.code == ReasonCode::unsupported_query);
}

FO_TEST(store_limits_are_enforced) {
  StoreLimits limits;
  limits.max_records = 3;
  EvidenceStore store(limits);
  register_pair(store);

  for (int index = 1; index <= 3; ++index) {
    const std::string load = std::to_string(index);
    FO_REQUIRE(store
                   .admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1,
                                        static_cast<std::uint64_t>(index), 1, load.c_str(), "watts", 100))
                   .retained);
  }
  FO_REQUIRE_EQ(store.record_count(), 3);
  const AdmissionOutcome refused =
      store.admit(make_evidence("dccp-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "1", "watts", 100));
  FO_REQUIRE(refused.code == ReasonCode::too_many_records);
  FO_REQUIRE(!refused.retained);
  FO_REQUIRE_EQ(store.record_count(), 3);
}

FO_TEST(store_subject_and_aspect_enumeration) {
  EvidenceStore store;
  register_pair(store);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.state", 1, 1, 1, "1", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "site:sea1", "power.total", 1, 1, 1, "1", "watts", 100))
                  .retained);

  const EntityRef rack = EntityRef::parse("rack:r1").value();
  FO_REQUIRE(store.knows_subject(rack));
  FO_REQUIRE_EQ(store.aspects_of(rack).size(), 2);
  FO_REQUIRE_EQ(store.subjects(Domain::rack).size(), 1);
  FO_REQUIRE_EQ(store.subjects(Domain::site).size(), 1);
  FO_REQUIRE_EQ(store.subjects().size(), 2);
  FO_REQUIRE(!store.knows_subject(EntityRef::parse("rack:missing").value()));
  FO_REQUIRE(store.aspects_of(EntityRef::parse("rack:missing").value()).empty());
  FO_REQUIRE(store.history(EntityRef::parse("rack:missing").value()).empty());
}

FO_TEST(store_plan_matches_commit) {
  Rng rng(0xD15EA5EULL);
  for (int round = 0; round < 300; ++round) {
    EvidenceStore store;
    register_pair(store);
    const std::uint64_t generation = static_cast<std::uint64_t>(1 + rng.below(6));
    const EvidenceRecord record =
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, generation, 1,
                      rng.coin() ? "1.5" : "1.6", "kilowatts", 100);
    const AdmissionOutcome planned = store.plan(record);
    FO_REQUIRE_EQ(store.record_count(), 0);
    const AdmissionOutcome committed = store.admit(record);
    FO_REQUIRE_MESSAGE(planned == committed, "planning and committing disagreed on round " + std::to_string(round));
  }
}
