// Facility Observatory - in-memory evidence store and evaluation engine.
//
// EvidenceStore is deliberately not thread safe. Concurrency ownership belongs
// to the Observatory facade, which holds exactly one lock for the whole store so
// that no read-to-write upgrade and no nested acquisition is possible.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_STORE_HPP
#define FACILITY_OBSERVATORY_STORE_HPP

#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/policy.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

// ---------------------------------------------------------------------------
// Bounded resource behaviour
// ---------------------------------------------------------------------------
struct FO_API StoreLimits {
  std::size_t max_records{1000000};
  std::size_t max_authorities{4096};
  std::size_t max_subjects{262144};
  std::size_t max_aspects_per_subject{1024};
  std::size_t max_explanation_bytes{1024};
};

// ---------------------------------------------------------------------------
// Admission
// ---------------------------------------------------------------------------
struct FO_API AdmissionOutcome {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  // The record became part of the current view.
  bool admitted{false};
  // The record was stored: as history, as a conflict participant, or both.
  bool retained{false};
  // The record is byte-for-byte the same publication as one already held.
  bool duplicate{false};
  // The generation fence advanced for this (authority, source, aspect, epoch).
  bool fence_advanced{false};
  Epoch effective_epoch{};
  Generation effective_generation{};
  Revision effective_revision{};

  friend bool operator==(const AdmissionOutcome&, const AdmissionOutcome&) = default;
};

// ---------------------------------------------------------------------------
// Aggregation
// ---------------------------------------------------------------------------
enum class Aggregation : std::uint8_t {
  sum = 0,
  minimum = 1,
  maximum = 2,
  count = 3,
  mean = 4,
};

FO_API std::string_view to_string(Aggregation aggregation) noexcept;
FO_API Outcome<Aggregation> aggregation_from_string(std::string_view text);

struct FO_API AggregateResult {
  EntityRef scope{};
  AspectId aspect{};
  Aggregation kind{Aggregation::sum};
  ObservationState state{ObservationState::unknown};
  ReasonCode code{ReasonCode::no_evidence};
  std::string detail{};
  std::optional<Value> value{};
  std::size_t expected_contributors{0};
  std::size_t observed_contributors{0};
  std::vector<EntityRef> missing{};
  std::vector<EvidenceRef> participants{};
  std::vector<EvidenceRef> contributors{};
  std::vector<std::optional<Reason>> participant_notes{};
};

// ---------------------------------------------------------------------------
// EvidenceStore
// ---------------------------------------------------------------------------
class FO_API EvidenceStore {
 public:
  EvidenceStore();
  explicit EvidenceStore(StoreLimits limits);

  EvidenceStore(const EvidenceStore&) = delete;
  EvidenceStore& operator=(const EvidenceStore&) = delete;
  EvidenceStore(EvidenceStore&&) = delete;
  EvidenceStore& operator=(EvidenceStore&&) = delete;
  ~EvidenceStore() = default;

  // --- authority registry ------------------------------------------------
  Outcome<AuthorityDescriptor> register_authority(const AuthorityDescriptor& descriptor);
  // The same decision without any mutation. The Observatory uses the planning
  // path to obtain a durable commit before the in-memory view changes, so a
  // crash can never leave the store ahead of storage.
  Outcome<AuthorityDescriptor> plan_register_authority(const AuthorityDescriptor& descriptor) const;
  Outcome<AuthorityDescriptor> authority(const AuthorityId& id) const;
  [[nodiscard]] std::vector<AuthorityDescriptor> authorities() const;
  [[nodiscard]] bool knows_authority(const AuthorityId& id) const;

  // Advances an authority's epoch explicitly. Every record from an earlier
  // epoch immediately becomes historical. An epoch that does not advance is
  // refused rather than silently ignored.
  AdmissionOutcome advance_epoch(const AuthorityId& id, Epoch epoch, Timestamp published_at);
  AdmissionOutcome plan_advance_epoch(const AuthorityId& id, Epoch epoch, Timestamp published_at) const;

  // --- admission ---------------------------------------------------------
  AdmissionOutcome admit(EvidenceRecord record);
  AdmissionOutcome plan(const EvidenceRecord& record) const;

  // --- evaluation --------------------------------------------------------
  [[nodiscard]] Evaluation evaluate(const EntityRef& entity, const AspectId& aspect, Timestamp at) const;

  [[nodiscard]] AggregateResult aggregate(const EntityRef& scope, const std::vector<EntityRef>& subjects,
                                          const AspectId& aspect, Aggregation kind, bool require_complete,
                                          Timestamp at) const;

  // --- inspection --------------------------------------------------------
  [[nodiscard]] bool knows_subject(const EntityRef& entity) const;
  [[nodiscard]] std::vector<AspectId> aspects_of(const EntityRef& entity) const;
  [[nodiscard]] std::vector<EntityRef> subjects() const;
  [[nodiscard]] std::vector<EntityRef> subjects(Domain domain) const;
  [[nodiscard]] std::vector<EvidenceRecord> history(const EntityRef& entity, const AspectId& aspect) const;
  [[nodiscard]] std::vector<EvidenceRecord> history(const EntityRef& entity) const;
  [[nodiscard]] std::vector<EvidenceRecord> records() const;

  // --- capacity / identity ----------------------------------------------
  [[nodiscard]] std::size_t record_count() const noexcept { return records_.size(); }
  [[nodiscard]] std::size_t authority_count() const noexcept { return authorities_.size(); }
  [[nodiscard]] std::size_t subject_count() const noexcept { return by_subject_.size(); }
  [[nodiscard]] const StoreLimits& limits() const noexcept { return limits_; }
  void set_limits(const StoreLimits& limits) noexcept { limits_ = limits; }
  void clear();

  // --- policy ------------------------------------------------------------
  [[nodiscard]] PolicySet& policies() noexcept { return policies_; }
  [[nodiscard]] const PolicySet& policies() const noexcept { return policies_; }

  // Deterministic digest of the entire current content, independent of the
  // order in which records were admitted.
  [[nodiscard]] std::string digest() const;

 private:
  struct FenceKey {
    AuthorityId authority{};
    SourceId source{};
    AspectId aspect{};
    Epoch epoch{};

    friend bool operator==(const FenceKey&, const FenceKey&) = default;
    friend auto operator<=>(const FenceKey&, const FenceKey&) = default;
  };

  struct FenceState {
    Generation generation{};
    Revision revision{};

    friend bool operator==(const FenceState&, const FenceState&) = default;
  };

  using RecordIndex = std::size_t;

  Outcome<AuthorityDescriptor> register_authority_impl(const AuthorityDescriptor& descriptor, bool commit);
  AdmissionOutcome advance_epoch_impl(const AuthorityId& id, Epoch epoch, Timestamp published_at, bool commit);
  AdmissionOutcome admit_impl(EvidenceRecord record, bool commit);

  StoreLimits limits_{};
  PolicySet policies_{};
  std::vector<EvidenceRecord> records_{};
  std::map<EvidenceKey, std::vector<RecordIndex>> by_key_{};
  std::map<EntityRef, std::map<AspectId, std::vector<RecordIndex>>> by_subject_{};
  std::map<FenceKey, FenceState> fences_{};
  std::map<AuthorityId, AuthorityDescriptor> authorities_{};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_STORE_HPP
