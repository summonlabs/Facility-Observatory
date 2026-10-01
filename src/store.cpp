// Facility Observatory - in-memory evidence store and evaluation engine.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/store.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "facility_observatory/checked.hpp"
#include "facility_observatory/integrity.hpp"

namespace fo {
namespace {

// One considered record plus the disposition the evaluator reached for it.
struct CandidateState {
  std::size_t record_index{0};
  EvidenceRef reference{};
  bool current{false};
  bool fresh{false};
  std::optional<Reason> note{};
  std::optional<Value> normalized{};
};

std::string describe_note_counts(const std::vector<CandidateState>& states) {
  std::map<ReasonCode, std::size_t> counts;
  for (const CandidateState& state : states) {
    if (state.note.has_value()) {
      counts[state.note->code] += 1U;
    }
  }
  if (counts.empty()) {
    return std::string("no participants were rejected");
  }
  std::string summary;
  for (const auto& entry : counts) {
    if (!summary.empty()) {
      summary.append(", ");
    }
    summary.append(to_string(entry.first));
    summary.push_back('=');
    summary.append(std::to_string(entry.second));
  }
  return summary;
}

std::string canonical_record_line(const EvidenceRecord& record) {
  std::string line;
  line.reserve(256);
  line.append(record.key.authority.str());
  line.push_back('/');
  line.append(record.key.source.str());
  line.push_back('|');
  line.append(record.key.entity.to_string());
  line.push_back('|');
  line.append(record.key.aspect.str());
  line.append("|e=");
  line.append(std::to_string(record.epoch.value()));
  line.append("|g=");
  line.append(std::to_string(record.generation.value()));
  line.append("|r=");
  line.append(std::to_string(record.revision.value()));
  line.append("|obs=");
  line.append(record.observed_at.to_rfc3339());
  line.append("|pub=");
  line.append(record.published_at.to_rfc3339());
  line.append("|v=");
  line.append(record.value.to_string());
  // Durability and journal sequence describe our handling of the claim, not the
  // claim itself, so they are deliberately excluded: the digest is then a pure
  // function of the retained evidence set and survives a restart unchanged.
  line.append("|x=");
  line.append(record.explanation);
  line.push_back('\n');
  return line;
}

std::string canonical_authority_line(const AuthorityDescriptor& descriptor) {
  std::string line;
  line.append(descriptor.id.str());
  line.push_back('|');
  line.append(to_string(descriptor.kind));
  line.push_back('|');
  line.append(to_string(descriptor.role));
  line.append("|epoch=");
  line.append(std::to_string(descriptor.epoch.value()));
  line.append("|synthetic=");
  line.append(descriptor.synthetic ? "1" : "0");
  line.push_back('\n');
  return line;
}

}  // namespace

std::string_view to_string(Aggregation aggregation) noexcept {
  switch (aggregation) {
    case Aggregation::sum:
      return std::string_view{"sum"};
    case Aggregation::minimum:
      return std::string_view{"minimum"};
    case Aggregation::maximum:
      return std::string_view{"maximum"};
    case Aggregation::count:
      return std::string_view{"count"};
    case Aggregation::mean:
      return std::string_view{"mean"};
  }
  return std::string_view{"unknown"};
}

Outcome<Aggregation> aggregation_from_string(std::string_view text) {
  const std::array<std::pair<std::string_view, Aggregation>, 5> table{{
      {"sum", Aggregation::sum},
      {"minimum", Aggregation::minimum},
      {"maximum", Aggregation::maximum},
      {"count", Aggregation::count},
      {"mean", Aggregation::mean},
  }};
  for (const auto& entry : table) {
    if (entry.first == text) {
      return Outcome<Aggregation>::ok(entry.second);
    }
  }
  return Outcome<Aggregation>::fail(ReasonCode::invalid_argument,
                                    "unknown aggregation '" + std::string(text) + "'");
}

EvidenceStore::EvidenceStore() = default;

EvidenceStore::EvidenceStore(StoreLimits limits) : limits_(limits) {}

void EvidenceStore::clear() {
  records_.clear();
  by_key_.clear();
  by_subject_.clear();
  fences_.clear();
  authorities_.clear();
}

// ---------------------------------------------------------------------------
// Authority registry
// ---------------------------------------------------------------------------
Outcome<AuthorityDescriptor> EvidenceStore::register_authority(const AuthorityDescriptor& descriptor) {
  return register_authority_impl(descriptor, true);
}

Outcome<AuthorityDescriptor> EvidenceStore::plan_register_authority(
    const AuthorityDescriptor& descriptor) const {
  // Decision-only path. register_authority_impl(descriptor, false) performs no
  // mutation whatsoever, which is what makes this const cast sound; keeping one
  // implementation is what stops planning and committing from drifting apart.
  return const_cast<EvidenceStore*>(this)->register_authority_impl(descriptor, false);
}

Outcome<AuthorityDescriptor> EvidenceStore::register_authority_impl(const AuthorityDescriptor& descriptor,
                                                                   bool commit) {
  if (descriptor.id.empty()) {
    return Outcome<AuthorityDescriptor>::fail(ReasonCode::empty_input, "authority identifier is empty");
  }
  auto existing = authorities_.find(descriptor.id);
  if (existing == authorities_.end()) {
    if (authorities_.size() >= limits_.max_authorities) {
      return Outcome<AuthorityDescriptor>::fail(
          ReasonCode::too_many_records,
          "authority registry is at its bound of " + std::to_string(limits_.max_authorities) + " entries");
    }
    if (commit) {
      authorities_.emplace(descriptor.id, descriptor);
    }
    return Outcome<AuthorityDescriptor>::ok(descriptor);
  }

  AuthorityDescriptor current = existing->second;
  if (current.kind != descriptor.kind) {
    // Upgrade from an unknown family is allowed once; a change between two known
    // families is refused, because that would silently transfer authority.
    if (current.kind != AuthorityKind::unknown) {
      return Outcome<AuthorityDescriptor>::fail(
          ReasonCode::authority_boundary_violation,
          "authority '" + descriptor.id.str() + "' is registered as '" + std::string(to_string(current.kind)) +
              "' and cannot be re-registered as '" + std::string(to_string(descriptor.kind)) + "'");
    }
    current.kind = descriptor.kind;
  }
  if (descriptor.role != AuthorityRole::unknown) {
    current.role = descriptor.role;
  }
  if (!descriptor.label.empty()) {
    current.label = descriptor.label;
  }
  if (descriptor.synthetic) {
    current.synthetic = true;
  }
  if (descriptor.epoch > current.epoch) {
    current.epoch = descriptor.epoch;
    current.epoch_published_at = descriptor.epoch_published_at;
  } else if (descriptor.epoch < current.epoch && !descriptor.epoch.is_unset()) {
    return Outcome<AuthorityDescriptor>::fail(
        ReasonCode::stale_epoch,
        "authority '" + descriptor.id.str() + "' is at epoch " + std::to_string(current.epoch.value()) +
            " and cannot be re-registered at epoch " + std::to_string(descriptor.epoch.value()));
  }
  if (commit) {
    existing->second = current;
  }
  return Outcome<AuthorityDescriptor>::ok(current);
}

Outcome<AuthorityDescriptor> EvidenceStore::authority(const AuthorityId& id) const {
  const auto found = authorities_.find(id);
  if (found == authorities_.end()) {
    return Outcome<AuthorityDescriptor>::fail(ReasonCode::authority_unknown,
                                              "authority '" + id.str() + "' is not registered");
  }
  return Outcome<AuthorityDescriptor>::ok(found->second);
}

std::vector<AuthorityDescriptor> EvidenceStore::authorities() const {
  std::vector<AuthorityDescriptor> result;
  result.reserve(authorities_.size());
  for (const auto& entry : authorities_) {
    result.push_back(entry.second);
  }
  return result;
}

bool EvidenceStore::knows_authority(const AuthorityId& id) const {
  return authorities_.find(id) != authorities_.end();
}

AdmissionOutcome EvidenceStore::advance_epoch(const AuthorityId& id, Epoch epoch, Timestamp published_at) {
  return advance_epoch_impl(id, epoch, published_at, true);
}

AdmissionOutcome EvidenceStore::plan_advance_epoch(const AuthorityId& id, Epoch epoch,
                                                   Timestamp published_at) const {
  return const_cast<EvidenceStore*>(this)->advance_epoch_impl(id, epoch, published_at, false);
}

AdmissionOutcome EvidenceStore::advance_epoch_impl(const AuthorityId& id, Epoch epoch, Timestamp published_at,
                                                   bool commit) {
  AdmissionOutcome outcome;
  const auto found = authorities_.find(id);
  if (found == authorities_.end()) {
    outcome.code = ReasonCode::authority_unknown;
    outcome.detail = "authority '" + id.str() + "' is not registered";
    return outcome;
  }
  if (epoch.is_unset()) {
    outcome.code = ReasonCode::invalid_argument;
    outcome.detail = "an epoch fence must be at least 1";
    outcome.effective_epoch = found->second.epoch;
    return outcome;
  }
  if (epoch <= found->second.epoch) {
    outcome.code = ReasonCode::replayed_generation;
    outcome.detail = "epoch " + std::to_string(epoch.value()) + " does not advance authority '" + id.str() +
                     "' beyond " + std::to_string(found->second.epoch.value());
    outcome.effective_epoch = found->second.epoch;
    return outcome;
  }
  if (commit) {
    found->second.epoch = epoch;
    found->second.epoch_published_at = published_at;
  }
  outcome.code = ReasonCode::ok;
  outcome.detail = "epoch advanced; every earlier record is now historical";
  outcome.admitted = true;
  outcome.fence_advanced = true;
  outcome.effective_epoch = epoch;
  return outcome;
}

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------
AdmissionOutcome EvidenceStore::admit(EvidenceRecord record) { return admit_impl(std::move(record), true); }

AdmissionOutcome EvidenceStore::plan(const EvidenceRecord& record) const {
  return const_cast<EvidenceStore*>(this)->admit_impl(record, false);
}

AdmissionOutcome EvidenceStore::admit_impl(EvidenceRecord record, bool commit) {
  AdmissionOutcome outcome;

  if (record.key.authority.empty() || record.key.source.empty() || record.key.aspect.empty() ||
      record.key.entity.id.empty()) {
    outcome.code = ReasonCode::invalid_identifier;
    outcome.detail = "evidence key is incomplete";
    return outcome;
  }
  if (record.explanation.size() > limits_.max_explanation_bytes) {
    outcome.code = ReasonCode::record_too_large;
    outcome.detail = "explanation of " + std::to_string(record.explanation.size()) + " bytes exceeds bound of " +
                     std::to_string(limits_.max_explanation_bytes) + " bytes";
    return outcome;
  }
  if (record.epoch.is_unset() || record.generation.is_unset() || record.revision.is_unset()) {
    outcome.code = ReasonCode::invalid_argument;
    outcome.detail = "epoch, generation and revision must all be at least 1";
    return outcome;
  }
  if (!record.observed_at.is_set() || !record.published_at.is_set()) {
    outcome.code = ReasonCode::indeterminate;
    outcome.detail = "observation and publication instants must both be set";
    return outcome;
  }
  if (record.published_at < record.observed_at) {
    outcome.code = ReasonCode::indeterminate;
    outcome.detail = "publication instant precedes observation instant";
    return outcome;
  }

  const auto authority_entry = authorities_.find(record.key.authority);
  if (authority_entry == authorities_.end()) {
    outcome.code = ReasonCode::authority_unknown;
    outcome.detail = "authority '" + record.key.authority.str() + "' is not registered";
    return outcome;
  }

  const bool subject_known = by_subject_.find(record.key.entity) != by_subject_.end();
  if (!subject_known && by_subject_.size() >= limits_.max_subjects) {
    outcome.code = ReasonCode::too_many_records;
    outcome.detail = "subject index is at its bound of " + std::to_string(limits_.max_subjects) + " entries";
    return outcome;
  }
  if (subject_known) {
    const auto subject_entry = by_subject_.find(record.key.entity);
    const bool aspect_known = subject_entry->second.find(record.key.aspect) != subject_entry->second.end();
    if (!aspect_known && subject_entry->second.size() >= limits_.max_aspects_per_subject) {
      outcome.code = ReasonCode::too_many_records;
      outcome.detail = "subject " + record.key.entity.to_string() + " already carries " +
                       std::to_string(subject_entry->second.size()) + " aspects";
      return outcome;
    }
  }

  // Idempotency: an identical publication is never stored twice, so the retained
  // set -- and therefore every derived view -- does not depend on arrival order.
  const auto key_entry = by_key_.find(record.key);
  if (key_entry != by_key_.end()) {
    for (const std::size_t index : key_entry->second) {
      // Recovered evidence never absorbs a live publication: the live record is
      // the refresh that promotes the subject out of its historical state.
      if (records_[index].provenance.durability == Durability::recovered &&
          record.provenance.durability != Durability::recovered) {
        continue;
      }
      if (same_publication(records_[index], record)) {
        outcome.code = ReasonCode::duplicate_evidence;
        outcome.duplicate = true;
        outcome.detail = "an identical publication from '" + record.key.authority.str() + "' is already held at " +
                         to_reference(records_[index]).to_string();
        outcome.effective_epoch = record.epoch;
        outcome.effective_generation = record.generation;
        outcome.effective_revision = record.revision;
        return outcome;
      }
    }
  }

  if (records_.size() >= limits_.max_records) {
    outcome.code = ReasonCode::too_many_records;
    outcome.detail = "record store is at its bound of " + std::to_string(limits_.max_records) + " records";
    return outcome;
  }

  const auto retain = [this, commit](const EvidenceRecord& value) -> std::size_t {
    if (!commit) {
      return 0;  // planning only: the decision is identical, nothing is stored
    }
    records_.push_back(value);
    const std::size_t index = records_.size() - 1U;
    by_key_[value.key].push_back(index);
    by_subject_[value.key.entity][value.key.aspect].push_back(index);
    return index;
  };

  AuthorityDescriptor& descriptor = authority_entry->second;

  if (record.epoch < descriptor.epoch) {
    retain(record);
    outcome.code = ReasonCode::stale_epoch;
    outcome.retained = true;
    outcome.effective_epoch = descriptor.epoch;
    outcome.effective_generation = record.generation;
    outcome.effective_revision = record.revision;
    outcome.detail = "epoch " + std::to_string(record.epoch.value()) + " is historical; authority '" +
                     record.key.authority.str() + "' is at epoch " + std::to_string(descriptor.epoch.value()) +
                     ", so this record is retained as history and cannot become current";
    return outcome;
  }
  if (record.epoch > descriptor.epoch) {
    if (commit) {
      descriptor.epoch = record.epoch;
      descriptor.epoch_published_at = record.published_at;
    }
    outcome.fence_advanced = true;
  }

  const FenceKey fence_key{record.key.authority, record.key.source, record.key.aspect, record.epoch};
  auto fence_entry = fences_.find(fence_key);
  if (fence_entry == fences_.end()) {
    if (commit) {
      fences_.emplace(fence_key, FenceState{record.generation, record.revision});
    }
    outcome.fence_advanced = true;
  } else {
    FenceState& fence = fence_entry->second;
    const bool superseded = record.generation < fence.generation ||
                            (record.generation == fence.generation && record.revision < fence.revision);
    if (superseded) {
      retain(record);
      outcome.code = ReasonCode::superseded_revision;
      outcome.retained = true;
      outcome.effective_epoch = record.epoch;
      outcome.effective_generation = fence.generation;
      outcome.effective_revision = fence.revision;
      outcome.detail = "generation " + std::to_string(record.generation.value()) + " revision " +
                       std::to_string(record.revision.value()) + " is superseded by generation " +
                       std::to_string(fence.generation.value()) + " revision " +
                       std::to_string(fence.revision.value()) + " in epoch " +
                       std::to_string(record.epoch.value());
      return outcome;
    }
    if (record.generation == fence.generation && record.revision == fence.revision) {
      retain(record);
      outcome.code = ReasonCode::conflicting_evidence;
      outcome.retained = true;
      outcome.effective_epoch = record.epoch;
      outcome.effective_generation = record.generation;
      outcome.effective_revision = record.revision;
      outcome.detail = "authority '" + record.key.authority.str() + "' published a different claim at the same "
                       "generation and revision; both claims are preserved";
      return outcome;
    }
    if (commit) {
      fence.generation = record.generation;
      fence.revision = record.revision;
    }
    outcome.fence_advanced = true;
  }

  retain(record);
  outcome.code = ReasonCode::ok;
  outcome.admitted = true;
  outcome.retained = true;
  outcome.effective_epoch = record.epoch;
  outcome.effective_generation = record.generation;
  outcome.effective_revision = record.revision;
  outcome.detail = "admitted as current at generation " + std::to_string(record.generation.value()) + " revision " +
                   std::to_string(record.revision.value());
  return outcome;
}

// ---------------------------------------------------------------------------
// Inspection helpers
// ---------------------------------------------------------------------------
bool EvidenceStore::knows_subject(const EntityRef& entity) const {
  return by_subject_.find(entity) != by_subject_.end();
}

std::vector<AspectId> EvidenceStore::aspects_of(const EntityRef& entity) const {
  std::vector<AspectId> result;
  const auto subject = by_subject_.find(entity);
  if (subject == by_subject_.end()) {
    return result;
  }
  result.reserve(subject->second.size());
  for (const auto& entry : subject->second) {
    result.push_back(entry.first);
  }
  return result;
}

std::vector<EntityRef> EvidenceStore::subjects() const {
  std::vector<EntityRef> result;
  result.reserve(by_subject_.size());
  for (const auto& entry : by_subject_) {
    result.push_back(entry.first);
  }
  return result;
}

std::vector<EntityRef> EvidenceStore::subjects(Domain domain) const {
  std::vector<EntityRef> result;
  for (const auto& entry : by_subject_) {
    if (entry.first.domain == domain) {
      result.push_back(entry.first);
    }
  }
  return result;
}

std::vector<EvidenceRecord> EvidenceStore::history(const EntityRef& entity, const AspectId& aspect) const {
  std::vector<EvidenceRecord> result;
  const auto subject = by_subject_.find(entity);
  if (subject == by_subject_.end()) {
    return result;
  }
  const auto entry = subject->second.find(aspect);
  if (entry == subject->second.end()) {
    return result;
  }
  result.reserve(entry->second.size());
  for (const std::size_t index : entry->second) {
    result.push_back(records_[index]);
  }
  std::sort(result.begin(), result.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) { return canonical_less(lhs, rhs); });
  return result;
}

std::vector<EvidenceRecord> EvidenceStore::history(const EntityRef& entity) const {
  std::vector<EvidenceRecord> result;
  const auto subject = by_subject_.find(entity);
  if (subject == by_subject_.end()) {
    return result;
  }
  for (const auto& entry : subject->second) {
    for (const std::size_t index : entry.second) {
      result.push_back(records_[index]);
    }
  }
  std::sort(result.begin(), result.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) { return canonical_less(lhs, rhs); });
  return result;
}

std::vector<EvidenceRecord> EvidenceStore::records() const {
  std::vector<EvidenceRecord> result = records_;
  std::sort(result.begin(), result.end(),
            [](const EvidenceRecord& lhs, const EvidenceRecord& rhs) { return canonical_less(lhs, rhs); });
  return result;
}

std::string EvidenceStore::digest() const {
  std::vector<const EvidenceRecord*> ordered;
  ordered.reserve(records_.size());
  for (const EvidenceRecord& record : records_) {
    ordered.push_back(&record);
  }
  std::sort(ordered.begin(), ordered.end(), [](const EvidenceRecord* lhs, const EvidenceRecord* rhs) {
    return canonical_less(*lhs, *rhs);
  });
  std::uint64_t hash = 0xCBF29CE484222325ULL;
  for (const EvidenceRecord* record : ordered) {
    hash = fnv1a64(canonical_record_line(*record), hash);
  }
  for (const auto& entry : authorities_) {
    hash = fnv1a64(canonical_authority_line(entry.second), hash);
  }
  return to_hex(hash);
}

// ---------------------------------------------------------------------------
// Evaluation
// ---------------------------------------------------------------------------
Evaluation EvidenceStore::evaluate(const EntityRef& entity, const AspectId& aspect, Timestamp at) const {
  Evaluation result;
  result.entity = entity;
  result.aspect = aspect;

  const AspectPolicy& policy = policies_.policy_for(aspect);

  std::vector<std::size_t> candidates;
  {
    const auto subject = by_subject_.find(entity);
    if (subject != by_subject_.end()) {
      const auto entry = subject->second.find(aspect);
      if (entry != subject->second.end()) {
        candidates = entry->second;
      }
    }
  }
  if (candidates.empty()) {
    result.state = ObservationState::unknown;
    result.code = ReasonCode::no_evidence;
    result.detail = "no authority has published aspect '" + aspect.str() + "' for " + entity.to_string();
    return result;
  }
  if (!at.is_set()) {
    result.state = ObservationState::indeterminate;
    result.code = ReasonCode::indeterminate;
    result.detail = "evaluation instant is not set";
    return result;
  }

  std::sort(candidates.begin(), candidates.end(), [this](std::size_t lhs, std::size_t rhs) {
    return canonical_less(records_[lhs], records_[rhs]);
  });

  std::vector<CandidateState> states;
  states.reserve(candidates.size());

  for (const std::size_t index : candidates) {
    const EvidenceRecord& record = records_[index];
    CandidateState state;
    state.record_index = index;
    state.reference = to_reference(record);

    const auto authority_entry = authorities_.find(record.key.authority);
    if (authority_entry == authorities_.end()) {
      state.note = Reason{ReasonCode::authority_unknown,
                          "authority '" + record.key.authority.str() + "' is not registered"};
      states.push_back(std::move(state));
      continue;
    }
    const AuthorityDescriptor& descriptor = authority_entry->second;
    if (record.epoch < descriptor.epoch) {
      state.note = Reason{ReasonCode::stale_epoch,
                          "record epoch " + std::to_string(record.epoch.value()) + " is historical; authority is at " +
                              std::to_string(descriptor.epoch.value())};
      states.push_back(std::move(state));
      continue;
    }
    if (record.epoch > descriptor.epoch) {
      state.note = Reason{ReasonCode::indeterminate,
                          "record epoch " + std::to_string(record.epoch.value()) +
                              " leads the registered authority epoch " +
                              std::to_string(descriptor.epoch.value())};
      states.push_back(std::move(state));
      continue;
    }

    const FenceKey fence_key{record.key.authority, record.key.source, record.key.aspect, record.epoch};
    const auto fence_entry = fences_.find(fence_key);
    if (fence_entry != fences_.end()) {
      const FenceState& fence = fence_entry->second;
      const bool superseded = record.generation < fence.generation ||
                              (record.generation == fence.generation && record.revision < fence.revision);
      if (superseded) {
        state.note = Reason{ReasonCode::superseded_revision,
                            "generation " + std::to_string(record.generation.value()) + " revision " +
                                std::to_string(record.revision.value()) + " is superseded by generation " +
                                std::to_string(fence.generation.value()) + " revision " +
                                std::to_string(fence.revision.value())};
        states.push_back(std::move(state));
        continue;
      }
    }

    if (record.provenance.durability == Durability::recovered &&
        policy.aspect_class == AspectClass::dynamic_measurement) {
      bool live_at_same_position = false;
      for (const std::size_t other : candidates) {
        const EvidenceRecord& candidate = records_[other];
        if (candidate.key == record.key && candidate.provenance.durability != Durability::recovered &&
            candidate.epoch == record.epoch && candidate.generation == record.generation &&
            candidate.revision == record.revision) {
          live_at_same_position = true;
          break;
        }
      }
      if (live_at_same_position) {
        state.note = Reason{
            ReasonCode::recovered_not_current,
            "recovered record is superseded by a live publication at the same generation and revision"};
        states.push_back(std::move(state));
        continue;
      }
      // Historical, but still the best evidence we hold: it contributes to the
      // answer and holds the whole view at stale, never at known.
      state.current = true;
      state.fresh = false;
      state.note = Reason{ReasonCode::recovered_not_current,
                          "recovered dynamic evidence stays historical until a live publication refreshes it"};
      states.push_back(std::move(state));
      continue;
    }

    const FreshnessAssessment freshness =
        assess_freshness(policy.freshness, record.observed_at, record.published_at, at);
    switch (freshness.verdict) {
      case FreshnessVerdict::unevaluable:
      case FreshnessVerdict::future_dated:
        state.note = Reason{freshness.code, freshness.detail};
        states.push_back(std::move(state));
        continue;
      case FreshnessVerdict::expired: {
        // Distinguish "this source has gone quiet for this aspect" from ordinary
        // ageing: the same authority is still publishing other aspects of the
        // same subject, so the source is alive but has stopped speaking here.
        bool authority_alive_for_subject = false;
        const auto subject_entry = by_subject_.find(record.key.entity);
        if (subject_entry != by_subject_.end()) {
          for (const auto& aspect_entry : subject_entry->second) {
            if (aspect_entry.first == record.key.aspect) {
              continue;
            }
            const AspectPolicy& other_policy = policies_.policy_for(aspect_entry.first);
            for (const std::size_t other : aspect_entry.second) {
              const EvidenceRecord& candidate = records_[other];
              if (candidate.key.authority != record.key.authority || candidate.epoch < descriptor.epoch) {
                continue;
              }
              const FreshnessAssessment other_freshness = assess_freshness(
                  other_policy.freshness, candidate.observed_at, candidate.published_at, at);
              if (other_freshness.verdict == FreshnessVerdict::fresh) {
                authority_alive_for_subject = true;
                break;
              }
            }
            if (authority_alive_for_subject) {
              break;
            }
          }
        }
        state.note = Reason{
            authority_alive_for_subject ? ReasonCode::source_disappeared : ReasonCode::expired_evidence,
            authority_alive_for_subject
                ? "authority '" + record.key.authority.str() +
                      "' is still publishing other aspects of " + record.key.entity.to_string() +
                      " but has stopped publishing this one: " + freshness.detail
                : freshness.detail};
        states.push_back(std::move(state));
        continue;
      }
      case FreshnessVerdict::stale:
        state.current = true;
        state.fresh = false;
        state.note = Reason{ReasonCode::stale_evidence, freshness.detail};
        break;
      case FreshnessVerdict::fresh:
        state.current = true;
        state.fresh = true;
        break;
    }
    states.push_back(std::move(state));
  }

  const auto publish_participants = [&result, &states]() {
    result.participants.clear();
    result.participant_notes.clear();
    result.participants.reserve(states.size());
    result.participant_notes.reserve(states.size());
    for (const CandidateState& state : states) {
      result.participants.push_back(state.reference);
      result.participant_notes.push_back(state.note);
    }
  };
  publish_participants();

  const auto finalize_without_current = [&result, &states](ReasonCode fallback) {
    bool indeterminate_present = false;
    bool unit_present = false;
    ReasonCode first = fallback;
    std::string first_detail;
    bool have_first = false;
    for (const CandidateState& state : states) {
      if (!state.note.has_value()) {
        continue;
      }
      if (!have_first) {
        first = state.note->code;
        first_detail = state.note->detail;
        have_first = true;
      }
      if (state.note->code == ReasonCode::indeterminate) {
        indeterminate_present = true;
      }
      if (state.note->code == ReasonCode::unit_incompatible) {
        unit_present = true;
      }
    }
    if (indeterminate_present) {
      result.state = ObservationState::indeterminate;
      result.code = ReasonCode::indeterminate;
    } else if (unit_present) {
      result.state = ObservationState::unsupported;
      result.code = ReasonCode::unit_incompatible;
    } else {
      result.state = ObservationState::stale;
      result.code = first;
    }
    result.detail = "no participant is current (" + describe_note_counts(states) + "); first reason: " +
                    std::string(to_string(first)) + (first_detail.empty() ? std::string{} : ": " + first_detail);
  };

  std::vector<CandidateState*> current;
  for (CandidateState& state : states) {
    if (state.current) {
      current.push_back(&state);
    }
  }
  if (current.empty()) {
    finalize_without_current(ReasonCode::stale_evidence);
    return result;
  }

  // Determine the reporting unit for scalar participants.
  bool unit_conflict = false;
  bool scalar_present = false;
  std::set<Unit> distinct_units;
  for (const CandidateState* state : current) {
    const Value& value = records_[state->record_index].value;
    if (value.has_scalar()) {
      scalar_present = true;
      distinct_units.insert(canonical_unit(value.unit()));
    }
  }
  Unit target_unit = Unit::none;
  if (policy.canonical_unit.has_value()) {
    target_unit = policy.canonical_unit.value();
    for (const CandidateState* state : current) {
      const Value& value = records_[state->record_index].value;
      if (value.has_scalar() && !units_compatible(value.unit(), target_unit)) {
        unit_conflict = true;
      }
    }
  } else if (distinct_units.size() == 1) {
    target_unit = *distinct_units.begin();
  } else if (distinct_units.size() > 1) {
    unit_conflict = true;
  }

  if (unit_conflict && scalar_present) {
    std::string units;
    for (const Unit unit : distinct_units) {
      if (!units.empty()) {
        units.append(", ");
      }
      units.append(to_string(unit));
    }
    result.state = ObservationState::unsupported;
    result.code = ReasonCode::unit_incompatible;
    result.detail = "participants report incompatible units (" + units + "); no sound comparison is possible";
    for (const CandidateState* state : current) {
      result.contributors.push_back(state->reference);
    }
    publish_participants();
    return result;
  }

  std::vector<CandidateState*> usable;
  for (CandidateState& state : states) {
    if (!state.current) {
      continue;
    }
    const Value& value = records_[state.record_index].value;
    if (value.has_scalar()) {
      auto converted = convert(value.scalar_magnitude(), value.unit(), target_unit);
      if (!converted) {
        state.current = false;
        state.note = Reason{converted.code(), converted.reason().detail};
        continue;
      }
      auto normalized = Value::make_scalar(converted.value().reduced(), target_unit);
      if (!normalized) {
        state.current = false;
        state.note = Reason{normalized.code(), normalized.reason().detail};
        continue;
      }
      state.normalized = normalized.value();
    } else {
      state.normalized = value;
    }
    usable.push_back(&state);
  }
  publish_participants();

  if (usable.empty()) {
    finalize_without_current(ReasonCode::unsupported_query);
    return result;
  }

  std::vector<ConflictingValue> claims;
  for (const CandidateState* state : usable) {
    const Value& value = state->normalized.value();
    bool seen = false;
    for (const ConflictingValue& claim : claims) {
      if (value_equivalent(claim.value, value)) {
        seen = true;
        break;
      }
    }
    if (!seen) {
      ConflictingValue claim;
      claim.value = value;
      claim.source = state->reference;
      claims.push_back(std::move(claim));
    }
  }

  bool has_fresh = false;
  for (const CandidateState* state : usable) {
    if (state->fresh) {
      has_fresh = true;
      break;
    }
  }

  const ConflictingValue* winner = nullptr;
  bool resolved_by_precedence = false;
  if (claims.size() <= 1) {
    winner = claims.empty() ? nullptr : &claims.front();
  } else if (!policy.precedence.empty()) {
    for (const AuthorityId& preferred : policy.precedence) {
      for (const CandidateState* state : usable) {
        if (state->reference.key.authority != preferred) {
          continue;
        }
        for (const ConflictingValue& claim : claims) {
          if (value_equivalent(claim.value, state->normalized.value())) {
            winner = &claim;
            break;
          }
        }
        if (winner != nullptr) {
          break;
        }
      }
      if (winner != nullptr) {
        break;
      }
    }
    resolved_by_precedence = winner != nullptr;
  }

  const bool disagreement = claims.size() > 1;
  if (disagreement && !resolved_by_precedence) {
    result.state = ObservationState::conflicting;
    result.code = ReasonCode::conflicting_evidence;
    result.detail = std::to_string(claims.size()) +
                    " distinct claims are current and no authority precedence is configured; every claim is preserved";
  } else if (has_fresh) {
    result.state = ObservationState::known;
    result.code = ReasonCode::ok;
    result.detail = disagreement
                        ? "authority precedence resolved " + std::to_string(claims.size()) +
                              " distinct current claims; the remaining disagreement is reported"
                        : "a single fresh claim is current";
  } else {
    result.state = ObservationState::stale;
    bool any_live = false;
    bool any_recovered = false;
    for (const CandidateState* state : usable) {
      if (state->reference.durability == Durability::recovered) {
        any_recovered = true;
      } else {
        any_live = true;
      }
    }
    result.code = (any_recovered && !any_live) ? ReasonCode::recovered_not_current : ReasonCode::stale_evidence;
    result.detail = disagreement
                        ? "authority precedence resolved " + std::to_string(claims.size()) +
                              " distinct claims, but every current participant is stale"
                        : "the only current claim is stale";
  }

  if (winner != nullptr && result.state != ObservationState::conflicting) {
    result.value = winner->value;
    for (const CandidateState* state : usable) {
      if (value_equivalent(*state->normalized, winner->value)) {
        result.contributors.push_back(state->reference);
      }
    }
  } else {
    for (const ConflictingValue& claim : claims) {
      result.contributors.push_back(claim.source);
    }
  }
  if (disagreement) {
    result.disagreement = claims;
  }
  return result;
}

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------
AggregateResult EvidenceStore::aggregate(const EntityRef& scope, const std::vector<EntityRef>& subjects_in,
                                         const AspectId& aspect, Aggregation kind, bool require_complete,
                                         Timestamp at) const {
  AggregateResult result;
  result.scope = scope;
  result.aspect = aspect;
  result.kind = kind;

  std::vector<EntityRef> subjects = subjects_in;
  std::sort(subjects.begin(), subjects.end());
  subjects.erase(std::unique(subjects.begin(), subjects.end()), subjects.end());
  result.expected_contributors = subjects.size();

  std::vector<Value> values;
  bool any_stale = false;

  for (const EntityRef& subject : subjects) {
    const Evaluation evaluation = evaluate(subject, aspect, at);
    for (std::size_t index = 0; index < evaluation.participants.size(); ++index) {
      result.participants.push_back(evaluation.participants[index]);
      result.participant_notes.push_back(index < evaluation.participant_notes.size()
                                             ? evaluation.participant_notes[index]
                                             : std::optional<Reason>{});
    }
    if (evaluation.state != ObservationState::known && evaluation.state != ObservationState::stale) {
      result.missing.push_back(subject);
      continue;
    }
    if (!evaluation.value.has_value()) {
      result.missing.push_back(subject);
      continue;
    }
    if (evaluation.state == ObservationState::stale) {
      any_stale = true;
    }
    values.push_back(evaluation.value.value());
    if (!evaluation.contributors.empty()) {
      result.contributors.push_back(evaluation.contributors.front());
    }
  }
  result.observed_contributors = values.size();

  if (values.empty()) {
    result.state = ObservationState::unknown;
    result.code = ReasonCode::no_evidence;
    result.detail = "none of the " + std::to_string(result.expected_contributors) +
                    " subjects has a current value for aspect '" + aspect.str() + "'";
    return result;
  }
  if (require_complete && result.observed_contributors < result.expected_contributors) {
    result.state = ObservationState::unknown;
    result.code = ReasonCode::no_evidence;
    result.detail = std::to_string(result.expected_contributors - result.observed_contributors) + " of " +
                    std::to_string(result.expected_contributors) +
                    " subjects have no current value; a complete total cannot be produced";
    return result;
  }

  if (kind == Aggregation::count) {
    auto counted = FixedPoint::from_scaled(static_cast<std::int64_t>(result.observed_contributors), 0);
    if (!counted) {
      result.state = ObservationState::unsupported;
      result.code = counted.code();
      result.detail = counted.reason().detail;
      return result;
    }
    auto value = Value::make_scalar(counted.value(), Unit::count);
    if (!value) {
      result.state = ObservationState::unsupported;
      result.code = value.code();
      result.detail = value.reason().detail;
      return result;
    }
    result.value = value.value();
    result.state = any_stale ? ObservationState::stale : ObservationState::known;
    result.code = any_stale ? ReasonCode::stale_evidence : ReasonCode::ok;
    result.detail = "counted " + std::to_string(result.observed_contributors) + " subjects";
    return result;
  }

  bool all_scalar = true;
  for (const Value& value : values) {
    if (!value.has_scalar()) {
      all_scalar = false;
      break;
    }
  }
  if (!all_scalar) {
    result.state = ObservationState::unsupported;
    result.code = ReasonCode::unsupported_query;
    result.detail = "aggregation '" + std::string(to_string(kind)) + "' requires scalar evidence";
    return result;
  }

  std::set<Unit> distinct_units;
  for (const Value& value : values) {
    distinct_units.insert(canonical_unit(value.unit()));
  }
  if (distinct_units.size() > 1) {
    std::string units;
    for (const Unit unit : distinct_units) {
      if (!units.empty()) {
        units.append(", ");
      }
      units.append(to_string(unit));
    }
    result.state = ObservationState::unsupported;
    result.code = ReasonCode::unit_incompatible;
    result.detail = "contributors report incompatible units (" + units + ")";
    return result;
  }
  const Unit target_unit = *distinct_units.begin();

  std::vector<FixedPoint> magnitudes;
  magnitudes.reserve(values.size());
  for (const Value& value : values) {
    auto converted = convert(value.scalar_magnitude(), value.unit(), target_unit);
    if (!converted) {
      result.state = ObservationState::unsupported;
      result.code = converted.code();
      result.detail = converted.reason().detail;
      return result;
    }
    magnitudes.push_back(converted.value());
  }

  FixedPoint accumulated = magnitudes.front();
  if (kind == Aggregation::sum || kind == Aggregation::mean) {
    for (std::size_t index = 1; index < magnitudes.size(); ++index) {
      auto total = accumulated.add(magnitudes[index]);
      if (!total) {
        result.state = ObservationState::unsupported;
        result.code = total.code();
        result.detail = "aggregate overflowed: " + total.reason().detail;
        return result;
      }
      accumulated = total.value();
    }
  } else {
    for (std::size_t index = 1; index < magnitudes.size(); ++index) {
      auto ordering = accumulated.compare(magnitudes[index]);
      if (!ordering) {
        result.state = ObservationState::unsupported;
        result.code = ordering.code();
        result.detail = ordering.reason().detail;
        return result;
      }
      const bool take = kind == Aggregation::minimum
                            ? ordering.value() == std::strong_ordering::greater
                            : ordering.value() == std::strong_ordering::less;
      if (take) {
        accumulated = magnitudes[index];
      }
    }
  }

  if (kind == Aggregation::mean) {
    auto averaged = accumulated.divide(static_cast<std::int64_t>(magnitudes.size()), RoundingMode::half_even);
    if (!averaged) {
      result.state = ObservationState::unsupported;
      result.code = averaged.code();
      result.detail = averaged.reason().detail;
      return result;
    }
    accumulated = averaged.value();
  }

  auto value = Value::make_scalar(accumulated.reduced(), target_unit);
  if (!value) {
    result.state = ObservationState::unsupported;
    result.code = value.code();
    result.detail = value.reason().detail;
    return result;
  }
  result.value = value.value();
  result.state = any_stale ? ObservationState::stale : ObservationState::known;
  result.code = any_stale ? ReasonCode::stale_evidence : ReasonCode::ok;
  result.detail = std::string(to_string(kind)) + " over " + std::to_string(result.observed_contributors) +
                  " of " + std::to_string(result.expected_contributors) + " subjects in " +
                  std::string(to_string(target_unit));
  return result;
}

}  // namespace fo
