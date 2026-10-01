// Facility Observatory - the public runtime facade.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/observatory.hpp"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "facility_observatory/lock_order.hpp"

namespace fo {
namespace {

// Set while a user event handler is running on this thread. A call back into
// the observatory from inside a handler is refused rather than deadlocked.
thread_local bool g_in_dispatch = false;

// RAII so that a handler which throws still clears the flag.
class DispatchGuard {
 public:
  DispatchGuard() { g_in_dispatch = true; }
  ~DispatchGuard() { g_in_dispatch = false; }
  DispatchGuard(const DispatchGuard&) = delete;
  DispatchGuard& operator=(const DispatchGuard&) = delete;
  DispatchGuard(DispatchGuard&&) = delete;
  DispatchGuard& operator=(DispatchGuard&&) = delete;
};

constexpr std::size_t kMaxEventLog = 4096;

struct WriteRequest {
  JournalRecordType type{JournalRecordType::evidence};
  std::vector<std::uint8_t> payload{};
  Timestamp at{};
  bool done{false};
  AppendResult result{};
};

Timestamp stamp_or_now(Timestamp at) { return at.is_set() ? at : SystemClock{}.now(); }

Status apply_record(EvidenceStore& store, const JournalRecord& record) {
  switch (record.type) {
    case JournalRecordType::authority_registered: {
      auto decoded = decode_authority(record.payload);
      if (!decoded) {
        return decoded.propagate<Nothing>();
      }
      auto applied = store.register_authority(decoded.value());
      if (!applied) {
        return applied.propagate<Nothing>();
      }
      return success();
    }
    case JournalRecordType::epoch_advanced: {
      AuthorityId id;
      Epoch epoch;
      Timestamp published_at;
      auto decoded = decode_epoch_advance(record.payload, id, epoch, published_at);
      if (!decoded) {
        return decoded;
      }
      // A refusal here is a legitimate outcome: the epoch may already be ahead.
      static_cast<void>(store.advance_epoch(id, epoch, published_at));
      return success();
    }
    case JournalRecordType::evidence: {
      auto decoded = decode_evidence(record.payload);
      if (!decoded) {
        return decoded.propagate<Nothing>();
      }
      EvidenceRecord recovered = decoded.value();
      // Recovered evidence is never promoted to current: it is marked recovered
      // and the evaluator decides, per aspect class, whether that is fatal.
      recovered.provenance.durability = Durability::recovered;
      recovered.provenance.sequence = record.sequence;
      if (record.committed_at.is_set()) {
        recovered.provenance.received_at = record.committed_at;
      }
      recovered.provenance.epoch_at_receipt = recovered.epoch;
      if (recovered.provenance.origin.empty() || recovered.provenance.origin == "live") {
        recovered.provenance.origin = "journal-recovery";
      }
      static_cast<void>(store.admit(std::move(recovered)));
      return success();
    }
    case JournalRecordType::policy_set: {
      auto decoded = decode_policy(record.payload);
      if (!decoded) {
        return decoded.propagate<Nothing>();
      }
      // An empty aspect is the default policy, which a replayed journal must
      // restore just as faithfully as a named one.
      if (decoded.value().aspect.empty()) {
        return store.policies().set_default(decoded.value());
      }
      return store.policies().set(decoded.value());
    }
    case JournalRecordType::snapshot_marker:
    case JournalRecordType::session_open:
    case JournalRecordType::session_close:
      return success();
  }
  return Status::fail(ReasonCode::malformed_encoding, "journal record type is not defined");
}

}  // namespace

struct Observatory::Impl {
  ObservatoryOptions options{};
  EvidenceStore store{};
  Journal journal{};
  mutable std::shared_mutex store_mutex{};
  std::mutex ingest_mutex{};
  mutable std::mutex journal_mutex{};
  std::condition_variable journal_cv{};
  std::condition_variable journal_done_cv{};
  std::thread writer{};
  WriteRequest* pending{nullptr};
  bool stopping{false};
  std::atomic<bool> running{false};
  std::vector<JournalRecord> committed{};
  mutable std::mutex event_mutex{};
  std::vector<ObservatoryEvent> events{};
  EventHandler handler{};
  bool has_handler{false};
  RecoveryReport recovery{};

  ~Impl() { shutdown_writer(); }

  void run_writer();
  AppendResult submit(JournalRecordType type, const std::vector<std::uint8_t>& payload, Timestamp at);
  void emit(const ObservatoryEvent& event);
  void shutdown_writer() noexcept;
};

void Observatory::Impl::run_writer() {
  for (;;) {
    WriteRequest* request = nullptr;
    {
      LockOrderGuard queue_guard(LockRank::journal_queue);
      std::unique_lock<std::mutex> lock(journal_mutex);
      journal_cv.wait(lock, [this] { return pending != nullptr || stopping; });
      if (pending == nullptr) {
        return;
      }
      request = pending;
    }

    AppendResult result;
    {
      LockOrderGuard writer_guard(LockRank::journal_writer);
      result = journal.append(request->type, request->payload, request->at);
    }

    {
      LockOrderGuard queue_guard(LockRank::journal_queue);
      std::unique_lock<std::mutex> lock(journal_mutex);
      if (result.code == ReasonCode::ok) {
        JournalRecord committed_record;
        committed_record.type = request->type;
        committed_record.sequence = result.sequence;
        committed_record.committed_at = request->at;
        committed_record.payload = request->payload;
        committed.push_back(std::move(committed_record));
      }
      request->result = result;
      request->done = true;
      pending = nullptr;
      journal_done_cv.notify_all();
    }
  }
}

AppendResult Observatory::Impl::submit(JournalRecordType type, const std::vector<std::uint8_t>& payload,
                                       Timestamp at) {
  AppendResult result;
  if (!running.load(std::memory_order_acquire)) {
    result.code = ReasonCode::shutting_down;
    result.detail = "the journal writer is not running";
    return result;
  }
  WriteRequest request;
  request.type = type;
  request.payload = payload;
  request.at = at;

  LockOrderGuard queue_guard(LockRank::journal_queue);
  std::unique_lock<std::mutex> lock(journal_mutex);
  if (stopping) {
    result.code = ReasonCode::shutting_down;
    result.detail = "the journal writer is stopping";
    return result;
  }
  pending = &request;
  journal_cv.notify_all();
  journal_done_cv.wait(lock, [this, &request] { return request.done || stopping; });
  if (!request.done) {
    pending = nullptr;
    result.code = ReasonCode::cancelled;
    result.detail = "the journal append was cancelled by shutdown";
    return result;
  }
  return request.result;
}

void Observatory::Impl::emit(const ObservatoryEvent& event) {
  EventHandler handler_copy;
  bool invoke = false;
  {
    LockOrderGuard dispatch_guard(LockRank::event_dispatch);
    std::unique_lock<std::mutex> lock(event_mutex);
    if (events.size() < kMaxEventLog) {
      events.push_back(event);
    }
    if (has_handler) {
      handler_copy = handler;
      invoke = true;
    }
  }
  // Deliberately outside every lock: a handler can never be called while the
  // observatory holds store, journal or ingest state.
  if (invoke && handler_copy) {
    const DispatchGuard guard;
    handler_copy(event);
  }
}

void Observatory::Impl::shutdown_writer() noexcept {
  {
    LockOrderGuard queue_guard(LockRank::journal_queue);
    std::unique_lock<std::mutex> lock(journal_mutex);
    if (!writer.joinable()) {
      stopping = true;
      running.store(false, std::memory_order_release);
      journal.close();
      return;
    }
    stopping = true;
    journal_cv.notify_all();
  }
  if (writer.joinable()) {
    writer.join();
  }
  running.store(false, std::memory_order_release);
  journal.close();
}

Observatory::Observatory() = default;

Observatory::~Observatory() = default;

Observatory::Observatory(Observatory&& other) noexcept = default;

Observatory& Observatory::operator=(Observatory&& other) noexcept = default;

bool Observatory::is_open() const noexcept {
  return impl_ != nullptr && impl_->running.load(std::memory_order_acquire);
}

Status Observatory::assert_operation(BoundaryOperation operation) { return assert_observer_operation(operation); }

Outcome<Observatory> Observatory::open(const ObservatoryOptions& options) {
  ObservatoryOptions effective = options;
  if (effective.topology.containment_aspect.empty() || effective.topology.published_total_aspect.empty() ||
      effective.topology.constituent_aspect.empty()) {
    auto conventions = TopologyConventions::make("topology.parent", "power.total", "power.draw");
    if (!conventions) {
      return conventions.propagate<Observatory>();
    }
    effective.topology = conventions.value();
  }
  if (effective.divergence.relative_tolerance.is_zero()) {
    auto query = DivergenceQuery::make_default();
    if (!query) {
      return query.propagate<Observatory>();
    }
    effective.divergence.relative_tolerance = query.value().relative_tolerance;
  }

  auto journal = Journal::open(effective.journal);
  if (!journal) {
    return journal.propagate<Observatory>();
  }

  Observatory observatory;
  observatory.impl_ = std::make_unique<Impl>();
  Impl& impl = *observatory.impl_;
  impl.options = effective;
  impl.store.set_limits(effective.limits);
  impl.journal = std::move(journal).value();
  impl.recovery = impl.journal.recovery();

  for (const JournalRecord& record : impl.journal.records()) {
    auto applied = apply_record(impl.store, record);
    if (!applied) {
      return Outcome<Observatory>::fail(
          ReasonCode::malformed_encoding,
          "durable record " + std::to_string(record.sequence.value()) + " could not be replayed: " +
              to_debug_string(applied.reason()));
    }
    impl.committed.push_back(record);
  }

  impl.running.store(true, std::memory_order_release);
  if (!effective.journal.read_only) {
    impl.writer = std::thread([&impl] { impl.run_writer(); });
    const Timestamp opened_at = stamp_or_now(effective.journal.created_at);
    const std::vector<std::uint8_t> opening = encode_session_marker(effective.origin + ":open", opened_at);
    AppendResult marker = impl.submit(JournalRecordType::session_open, opening, opened_at);
    if (marker.code != ReasonCode::ok) {
      return Outcome<Observatory>::fail(marker.code, "cannot record the session opening: " + marker.detail);
    }
  }
  return Outcome<Observatory>::ok(std::move(observatory));
}

Outcome<AuthorityDescriptor> Observatory::register_authority(const AuthorityDescriptor& descriptor) {
  if (!is_open()) {
    return Outcome<AuthorityDescriptor>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  if (g_in_dispatch) {
    return Outcome<AuthorityDescriptor>::fail(
        ReasonCode::internal_error, "a re-entrant observatory call from an event handler is refused");
  }
  Impl& impl = *impl_;
  LockOrderGuard ingest_guard(LockRank::ingest);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  auto planned = impl.store.plan_register_authority(descriptor);
  if (!planned) {
    return planned;
  }
  const Timestamp at = stamp_or_now(descriptor.epoch_published_at);
  const std::vector<std::uint8_t> payload = encode_authority(planned.value());
  const AppendResult appended = impl.submit(JournalRecordType::authority_registered, payload, at);
  if (appended.code != ReasonCode::ok) {
    return Outcome<AuthorityDescriptor>::fail(
        appended.code, "authority registration is not durable: " + appended.detail);
  }
  return impl.store.register_authority(planned.value());
}

Outcome<AuthorityDescriptor> Observatory::authority(const AuthorityId& id) const {
  if (!is_open()) {
    return Outcome<AuthorityDescriptor>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.authority(id);
}

std::vector<AuthorityDescriptor> Observatory::authorities() const {
  if (!is_open()) {
    return {};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.authorities();
}

IngestionOutcome Observatory::record_evidence(EvidenceRecord record) {
  IngestionOutcome outcome;
  if (!is_open()) {
    outcome.code = ReasonCode::shutting_down;
    outcome.detail = "the observatory is not open";
    return outcome;
  }
  if (g_in_dispatch) {
    outcome.code = ReasonCode::internal_error;
    outcome.detail = "a re-entrant observatory call from an event handler is refused";
    return outcome;
  }
  Impl& impl = *impl_;

  ObservatoryEvent event;
  {
    LockOrderGuard ingest_guard(LockRank::ingest);
    std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

    AdmissionOutcome planned;
    {
      LockOrderGuard store_guard(LockRank::store);
      std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
      planned = impl.store.plan(record);
    }

    outcome.admission = planned;
    outcome.code = planned.code;
    outcome.detail = planned.detail;
    event.code = planned.code;
    event.detail = planned.detail;
    event.evidence = to_reference(record);

    if (!planned.retained) {
      // Nothing in the retained evidence set changes, so there is nothing to
      // make durable: an exact duplicate, an unregistered authority or a shape
      // refusal never reaches storage.
      outcome.durable = false;
      return outcome;
    }

    // The durable commit happens first. Only once the frame is committed does
    // the in-memory view move, so a crash can never leave the store ahead of
    // storage.
    record.provenance.sequence = JournalSequence{};
    if (!record.provenance.received_at.is_set()) {
      record.provenance.received_at = record.published_at;
    }
    const std::vector<std::uint8_t> payload = encode_evidence(record);
    const AppendResult appended = impl.submit(JournalRecordType::evidence, payload, record.provenance.received_at);
    if (appended.code != ReasonCode::ok) {
      outcome.code = appended.code;
      outcome.detail = "the observation is not durable and was not applied: " + appended.detail;
      event.code = appended.code;
      event.detail = outcome.detail;
      impl.emit(event);
      return outcome;
    }
    record.provenance.sequence = appended.sequence;

    AdmissionOutcome admitted;
    {
      LockOrderGuard store_guard(LockRank::store);
      std::unique_lock<std::shared_mutex> store_lock(impl.store_mutex);
      admitted = impl.store.admit(record);
    }
    outcome.admission = admitted;
    outcome.sequence = appended.sequence;
    outcome.durable = true;
    outcome.code = admitted.code;
    outcome.detail = admitted.detail;
    event.code = admitted.code;
    event.detail = admitted.detail;
    event.evidence = to_reference(record);
    event.durable = true;
    if (!(admitted == planned)) {
      outcome.code = ReasonCode::internal_error;
      outcome.detail = "the admission decision changed between planning and commit: planned '" +
                       std::string(to_string(planned.code)) + "' but committed '" +
                       std::string(to_string(admitted.code)) + "'";
      event.code = outcome.code;
      event.detail = outcome.detail;
    }
  }
  impl.emit(event);
  return outcome;
}

IngestionOutcome Observatory::advance_epoch(const AuthorityId& id, Epoch epoch, Timestamp published_at) {
  IngestionOutcome outcome;
  if (!is_open()) {
    outcome.code = ReasonCode::shutting_down;
    outcome.detail = "the observatory is not open";
    return outcome;
  }
  if (g_in_dispatch) {
    outcome.code = ReasonCode::internal_error;
    outcome.detail = "a re-entrant observatory call from an event handler is refused";
    return outcome;
  }
  Impl& impl = *impl_;
  LockOrderGuard ingest_guard(LockRank::ingest);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  const AdmissionOutcome planned = impl.store.plan_advance_epoch(id, epoch, published_at);
  outcome.admission = planned;
  outcome.code = planned.code;
  outcome.detail = planned.detail;
  if (planned.code != ReasonCode::ok) {
    return outcome;
  }
  const Timestamp at = stamp_or_now(published_at);
  const std::vector<std::uint8_t> payload = encode_epoch_advance(id, epoch, at);
  const AppendResult appended = impl.submit(JournalRecordType::epoch_advanced, payload, at);
  if (appended.code != ReasonCode::ok) {
    outcome.code = appended.code;
    outcome.detail = "the epoch advance is not durable and was not applied: " + appended.detail;
    return outcome;
  }
  outcome.admission = impl.store.advance_epoch(id, epoch, published_at);
  outcome.sequence = appended.sequence;
  outcome.durable = true;
  outcome.code = outcome.admission.code;
  outcome.detail = outcome.admission.detail;
  return outcome;
}

Status Observatory::set_policy(const AspectPolicy& policy) {
  if (!is_open()) {
    return Status::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  if (g_in_dispatch) {
    return Status::fail(ReasonCode::internal_error,
                        "a re-entrant observatory call from an event handler is refused");
  }
  Impl& impl = *impl_;
  LockOrderGuard ingest_guard(LockRank::ingest);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  auto validated = validate_aspect_policy(policy);
  if (!validated) {
    return validated;
  }
  const Timestamp at = SystemClock{}.now();
  const std::vector<std::uint8_t> payload = encode_policy(policy);
  const AppendResult appended = impl.submit(JournalRecordType::policy_set, payload, at);
  if (appended.code != ReasonCode::ok) {
    return Status::fail(appended.code, "the policy is not durable and was not applied: " + appended.detail);
  }
  LockOrderGuard store_guard(LockRank::store);
  std::unique_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.policies().set(policy);
}

Status Observatory::set_default_policy(const AspectPolicy& policy) {
  if (!is_open()) {
    return Status::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  if (g_in_dispatch) {
    return Status::fail(ReasonCode::internal_error,
                        "a re-entrant observatory call from an event handler is refused");
  }
  Impl& impl = *impl_;
  LockOrderGuard ingest_guard(LockRank::ingest);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  auto validated = impl.store.policies().plan_default(policy);
  if (!validated) {
    return validated;
  }
  const Timestamp at = SystemClock{}.now();
  const std::vector<std::uint8_t> payload = encode_policy(policy);
  const AppendResult appended = impl.submit(JournalRecordType::policy_set, payload, at);
  if (appended.code != ReasonCode::ok) {
    return Status::fail(appended.code, "the default policy is not durable and was not applied: " + appended.detail);
  }
  LockOrderGuard store_guard(LockRank::store);
  std::unique_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.policies().set_default(policy);
}

std::vector<AspectPolicy> Observatory::policies() const {
  if (!is_open()) {
    return {};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.policies().explicit_policies();
}

PolicySet Observatory::policy_set() const {
  if (!is_open()) {
    return PolicySet{};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.policies();
}

Evaluation Observatory::evaluate(const EntityRef& entity, const AspectId& aspect, Timestamp at) const {
  Evaluation result;
  result.entity = entity;
  result.aspect = aspect;
  if (!is_open()) {
    result.state = ObservationState::indeterminate;
    result.code = ReasonCode::shutting_down;
    result.detail = "the observatory is not open";
    return result;
  }
  if (g_in_dispatch) {
    result.state = ObservationState::indeterminate;
    result.code = ReasonCode::internal_error;
    result.detail = "a re-entrant observatory call from an event handler is refused";
    return result;
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.evaluate(entity, aspect, at);
}

AggregateResult Observatory::aggregate(const EntityRef& scope, const std::vector<EntityRef>& subjects,
                                       const AspectId& aspect, Aggregation kind, bool require_complete,
                                       Timestamp at) const {
  if (!is_open()) {
    AggregateResult result;
    result.scope = scope;
    result.aspect = aspect;
    result.kind = kind;
    result.state = ObservationState::indeterminate;
    result.code = ReasonCode::shutting_down;
    result.detail = "the observatory is not open";
    return result;
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.aggregate(scope, subjects, aspect, kind, require_complete, at);
}

Outcome<std::vector<Divergence>> Observatory::divergences(Timestamp at) const {
  if (!is_open()) {
    return Outcome<std::vector<Divergence>>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  DivergenceDetector detector(impl.store, impl.options.topology, impl.options.divergence);
  return detector.detect(at);
}

Outcome<std::vector<Divergence>> Observatory::divergences_for(const EntityRef& entity, Timestamp at) const {
  if (!is_open()) {
    return Outcome<std::vector<Divergence>>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  DivergenceDetector detector(impl.store, impl.options.topology, impl.options.divergence);
  return detector.detect_for(entity, at);
}

Outcome<std::vector<EvidenceRecord>> Observatory::history(const EntityRef& entity, const AspectId& aspect) const {
  if (!is_open()) {
    return Outcome<std::vector<EvidenceRecord>>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return Outcome<std::vector<EvidenceRecord>>::ok(impl.store.history(entity, aspect));
}

Outcome<std::vector<EvidenceRecord>> Observatory::history(const EntityRef& entity) const {
  if (!is_open()) {
    return Outcome<std::vector<EvidenceRecord>>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return Outcome<std::vector<EvidenceRecord>>::ok(impl.store.history(entity));
}

std::vector<EntityRef> Observatory::subjects() const {
  if (!is_open()) {
    return {};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.subjects();
}

std::vector<EntityRef> Observatory::subjects(Domain domain) const {
  if (!is_open()) {
    return {};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.subjects(domain);
}

std::vector<AspectId> Observatory::aspects_of(const EntityRef& entity) const {
  if (!is_open()) {
    return {};
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.aspects_of(entity);
}

bool Observatory::knows_subject(const EntityRef& entity) const {
  if (!is_open()) {
    return false;
  }
  const Impl& impl = *impl_;
  LockOrderGuard store_guard(LockRank::store);
  std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
  return impl.store.knows_subject(entity);
}

Outcome<Snapshot> Observatory::snapshot(Timestamp at) {
  if (!is_open()) {
    return Outcome<Snapshot>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  if (g_in_dispatch) {
    return Outcome<Snapshot>::fail(ReasonCode::internal_error,
                                   "a re-entrant observatory call from an event handler is refused");
  }
  Impl& impl = *impl_;
  LockOrderGuard ingest_guard(LockRank::ingest);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  Snapshot result;
  {
    LockOrderGuard store_guard(LockRank::store);
    std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
    result.digest = impl.store.digest();
    result.record_count = impl.store.record_count();
    result.authority_count = impl.store.authority_count();
    result.subject_count = impl.store.subject_count();
    result.policy_count = impl.store.policies().size();
  }
  result.created_at = stamp_or_now(at);
  {
    LockOrderGuard queue_guard(LockRank::journal_queue);
    std::unique_lock<std::mutex> journal_lock(impl.journal_mutex);
    const std::uint64_t next = impl.journal.next_sequence().value();
    result.sequence = JournalSequence::from_value(next == 0 ? 0 : next - 1);
  }
  const std::vector<std::uint8_t> payload =
      encode_snapshot_marker(result.sequence, result.digest, result.created_at);
  const AppendResult appended = impl.submit(JournalRecordType::snapshot_marker, payload, result.created_at);
  if (appended.code != ReasonCode::ok) {
    return Outcome<Snapshot>::fail(appended.code, "the snapshot marker is not durable: " + appended.detail);
  }
  return Outcome<Snapshot>::ok(std::move(result));
}

Outcome<Snapshot> Observatory::reconstruct(JournalSequence upto, Timestamp at) const {
  if (!is_open()) {
    return Outcome<Snapshot>::fail(ReasonCode::shutting_down, "the observatory is not open");
  }
  const Impl& impl = *impl_;
  std::vector<JournalRecord> selected;
  {
    LockOrderGuard queue_guard(LockRank::journal_queue);
    std::unique_lock<std::mutex> journal_lock(impl.journal_mutex);
    for (const JournalRecord& record : impl.committed) {
      if (record.sequence <= upto) {
        selected.push_back(record);
      }
    }
  }

  EvidenceStore rebuilt;
  rebuilt.set_limits(impl.options.limits);
  for (const JournalRecord& record : selected) {
    auto applied = apply_record(rebuilt, record);
    if (!applied) {
      return Outcome<Snapshot>::fail(ReasonCode::malformed_encoding,
                                     "durable record " + std::to_string(record.sequence.value()) +
                                         " could not be replayed for reconstruction: " +
                                         to_debug_string(applied.reason()));
    }
  }
  Snapshot result;
  result.sequence = upto;
  result.created_at = stamp_or_now(at);
  result.digest = rebuilt.digest();
  result.record_count = rebuilt.record_count();
  result.authority_count = rebuilt.authority_count();
  result.subject_count = rebuilt.subject_count();
  result.policy_count = rebuilt.policies().size();
  return Outcome<Snapshot>::ok(std::move(result));
}

ObservatoryStatus Observatory::status() const {
  ObservatoryStatus result;
  if (!is_open()) {
    return result;
  }
  const Impl& impl = *impl_;
  result.open = true;
  result.journal_path = impl.journal.path();
  result.recovery = impl.recovery;
  result.lock_order_violations = LockOrderAudit::violations();
  {
    LockOrderGuard queue_guard(LockRank::journal_queue);
    std::unique_lock<std::mutex> journal_lock(impl.journal_mutex);
    result.journal_records = static_cast<std::uint64_t>(impl.committed.size());
  }
  {
    LockOrderGuard store_guard(LockRank::store);
    std::shared_lock<std::shared_mutex> store_lock(impl.store_mutex);
    result.records = impl.store.record_count();
    result.authorities = impl.store.authority_count();
    result.subjects = impl.store.subject_count();
    result.policies = impl.store.policies().size();
    result.digest = impl.store.digest();
  }
  result.journal_bytes = impl.journal.committed_bytes();
  return result;
}

void Observatory::set_event_handler(EventHandler handler) {
  if (impl_ == nullptr) {
    return;
  }
  Impl& impl = *impl_;
  LockOrderGuard dispatch_guard(LockRank::event_dispatch);
  std::unique_lock<std::mutex> lock(impl.event_mutex);
  impl.handler = std::move(handler);
  impl.has_handler = static_cast<bool>(impl.handler);
}

std::vector<ObservatoryEvent> Observatory::drain_events() {
  if (impl_ == nullptr) {
    return {};
  }
  Impl& impl = *impl_;
  LockOrderGuard dispatch_guard(LockRank::event_dispatch);
  std::unique_lock<std::mutex> lock(impl.event_mutex);
  std::vector<ObservatoryEvent> drained = std::move(impl.events);
  impl.events.clear();
  return drained;
}

Status Observatory::close() {
  if (!is_open()) {
    return success();
  }
  Impl& impl = *impl_;
  LockOrderGuard lifecycle_guard(LockRank::lifecycle);
  std::unique_lock<std::mutex> ingest_lock(impl.ingest_mutex);

  if (!impl.options.journal.read_only) {
    const Timestamp at = SystemClock{}.now();
    const std::vector<std::uint8_t> payload = encode_session_marker(impl.options.origin + ":close", at);
    const AppendResult appended = impl.submit(JournalRecordType::session_close, payload, at);
    static_cast<void>(appended);
  }

  impl.shutdown_writer();
  return success();
}

}  // namespace fo
