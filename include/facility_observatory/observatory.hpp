// Facility Observatory - the public runtime facade.
//
// Concurrency ownership, stated once and enforced by LockOrderAudit:
//
//   * one lock protects the evidence store, and it is always the innermost
//     (LockRank::store); readers take it shared, never upgrading;
//   * every mutation is serialised by the ingest lock, which is always taken
//     before the store lock;
//   * the journal is touched only by the writer thread, which holds no other
//     lock while appending;
//   * event handlers are invoked after every lock has been released, and a
//     re-entrant call from inside a handler is refused instead of deadlocking.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_OBSERVATORY_HPP
#define FACILITY_OBSERVATORY_OBSERVATORY_HPP

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/divergence.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/journal.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/policy.hpp"
#include "facility_observatory/store.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

struct FO_API ObservatoryOptions {
  JournalOptions journal{};
  StoreLimits limits{};
  TopologyConventions topology{};
  DivergenceQuery divergence{};
  std::string origin{"facility-observatory"};
};

// The result of offering an observation to the runtime.
struct FO_API IngestionOutcome {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  AdmissionOutcome admission{};
  JournalSequence sequence{};
  // True only when the change reached its durable commit point before the
  // in-memory view changed.
  bool durable{false};
};

struct FO_API ObservatoryEvent {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  EvidenceRef evidence{};
  bool durable{false};
};

struct FO_API Snapshot {
  JournalSequence sequence{};
  Timestamp created_at{};
  std::string digest{};
  std::size_t record_count{0};
  std::size_t authority_count{0};
  std::size_t subject_count{0};
  std::size_t policy_count{0};
};

struct FO_API ObservatoryStatus {
  bool open{false};
  std::size_t records{0};
  std::size_t authorities{0};
  std::size_t subjects{0};
  std::size_t policies{0};
  std::uint64_t journal_records{0};
  std::uint64_t journal_bytes{0};
  std::string digest{};
  std::string journal_path{};
  RecoveryReport recovery{};
  std::uint64_t lock_order_violations{0};
};

using EventHandler = std::function<void(const ObservatoryEvent&)>;

class FO_API Observatory {
 public:
  Observatory();
  ~Observatory();
  Observatory(Observatory&& other) noexcept;
  Observatory& operator=(Observatory&& other) noexcept;
  Observatory(const Observatory&) = delete;
  Observatory& operator=(const Observatory&) = delete;

  // Opens (or creates) the journal, takes the single-writer lock, recovers a
  // torn tail conservatively, refuses interior corruption, replays durable
  // evidence as recovered, and starts the journal writer thread.
  static Outcome<Observatory> open(const ObservatoryOptions& options);

  [[nodiscard]] bool is_open() const noexcept;

  // Refuses any operation the observatory does not own.
  [[nodiscard]] static Status assert_operation(BoundaryOperation operation);

  // --- registry ----------------------------------------------------------
  Outcome<AuthorityDescriptor> register_authority(const AuthorityDescriptor& descriptor);
  Outcome<AuthorityDescriptor> authority(const AuthorityId& id) const;
  [[nodiscard]] std::vector<AuthorityDescriptor> authorities() const;

  // --- observation -------------------------------------------------------
  IngestionOutcome record_evidence(EvidenceRecord record);
  IngestionOutcome advance_epoch(const AuthorityId& id, Epoch epoch, Timestamp published_at);

  // --- policy ------------------------------------------------------------
  Status set_policy(const AspectPolicy& policy);
  Status set_default_policy(const AspectPolicy& policy);
  [[nodiscard]] std::vector<AspectPolicy> policies() const;
  // A consistent snapshot of the whole policy set, including the default.
  [[nodiscard]] PolicySet policy_set() const;

  // --- queries -----------------------------------------------------------
  [[nodiscard]] Evaluation evaluate(const EntityRef& entity, const AspectId& aspect, Timestamp at) const;
  [[nodiscard]] AggregateResult aggregate(const EntityRef& scope, const std::vector<EntityRef>& subjects,
                                          const AspectId& aspect, Aggregation kind, bool require_complete,
                                          Timestamp at) const;
  [[nodiscard]] Outcome<std::vector<Divergence>> divergences(Timestamp at) const;
  [[nodiscard]] Outcome<std::vector<Divergence>> divergences_for(const EntityRef& entity, Timestamp at) const;
  [[nodiscard]] Outcome<std::vector<EvidenceRecord>> history(const EntityRef& entity,
                                                             const AspectId& aspect) const;
  [[nodiscard]] Outcome<std::vector<EvidenceRecord>> history(const EntityRef& entity) const;
  [[nodiscard]] std::vector<EntityRef> subjects() const;
  [[nodiscard]] std::vector<EntityRef> subjects(Domain domain) const;
  [[nodiscard]] std::vector<AspectId> aspects_of(const EntityRef& entity) const;
  [[nodiscard]] bool knows_subject(const EntityRef& entity) const;

  // --- snapshot and reconstruction ---------------------------------------
  Outcome<Snapshot> snapshot(Timestamp at);
  Outcome<Snapshot> reconstruct(JournalSequence upto, Timestamp at) const;

  [[nodiscard]] ObservatoryStatus status() const;

  // --- events ------------------------------------------------------------
  void set_event_handler(EventHandler handler);
  [[nodiscard]] std::vector<ObservatoryEvent> drain_events();

  // --- lifecycle ---------------------------------------------------------
  Status close();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_{};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_OBSERVATORY_HPP
