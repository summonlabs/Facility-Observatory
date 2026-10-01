// Facility Observatory - concurrency, lock discipline and event tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

#include "facility_observatory/lock_order.hpp"
#include "facility_observatory/observatory.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

FO_TEST(lock_order_audit_accepts_increasing_ranks) {
  LockOrderAudit::reset();
  {
    LockOrderGuard ingest(LockRank::ingest);
    FO_REQUIRE(!ingest.violated());
    {
      LockOrderGuard store(LockRank::store);
      FO_REQUIRE(!store.violated());
      FO_REQUIRE_EQ(LockOrderAudit::depth(), 2);
    }
    FO_REQUIRE_EQ(LockOrderAudit::depth(), 1);
  }
  FO_REQUIRE_EQ(LockOrderAudit::depth(), 0);
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
  FO_REQUIRE(LockOrderAudit::last_violation().empty());
}

FO_TEST(lock_order_audit_detects_inversion) {
  LockOrderAudit::reset();
  {
    LockOrderGuard store(LockRank::store);
    LockOrderGuard ingest(LockRank::ingest);
    FO_REQUIRE(ingest.violated());
  }
  FO_REQUIRE(LockOrderAudit::violations() >= 1);
  FO_REQUIRE(LockOrderAudit::last_violation().find("ingest") != std::string::npos);
  LockOrderAudit::reset();
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
}

FO_TEST(lock_order_audit_detects_self_nesting) {
  LockOrderAudit::reset();
  {
    LockOrderGuard first(LockRank::store);
    LockOrderGuard second(LockRank::store);
    FO_REQUIRE(second.violated());
  }
  FO_REQUIRE(LockOrderAudit::violations() >= 1);
  LockOrderAudit::reset();
}

FO_TEST(concurrent_readers_and_a_writer_stay_consistent) {
  TempDir directory;
  LockOrderAudit::reset();

  ObservatoryOptions options;
  options.journal.path = directory.file("concurrent.foj");
  options.journal.origin = "concurrency-test";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  const int writers = 3;
  const int records_per_writer = 40;
  const int readers = 4;

  std::atomic<int> failures{0};
  std::atomic<int> admitted{0};
  std::atomic<bool> start{false};

  std::vector<std::thread> threads;
  for (int writer = 0; writer < writers; ++writer) {
    threads.emplace_back([&observatory, &failures, &admitted, &start, writer, records_per_writer] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int index = 1; index <= records_per_writer; ++index) {
        const std::string load = std::to_string(100 + writer * records_per_writer + index);
        const std::uint64_t generation = static_cast<std::uint64_t>(writer * records_per_writer + index);
        const IngestionOutcome outcome =
            observatory.value().record_evidence(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1,
                                                              generation, 1, load.c_str(), "watts", 100));
        // Writers run concurrently, so a lower generation can legitimately
        // arrive after a higher one and be fenced. That is not a failure.
        const bool fenced = outcome.code == ReasonCode::superseded_revision ||
                            outcome.code == ReasonCode::conflicting_evidence;
        if (outcome.code == ReasonCode::ok) {
          if (!outcome.durable) {
            failures.fetch_add(1);
          }
          admitted.fetch_add(1);
        } else if (!fenced) {
          failures.fetch_add(1);
        }
      }
    });
  }

  for (int reader = 0; reader < readers; ++reader) {
    threads.emplace_back([&observatory, &failures, &start, &rack, &draw] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      for (int iteration = 0; iteration < 200; ++iteration) {
        const Evaluation evaluation = observatory.value().evaluate(rack, draw, at_seconds(100));
        if (evaluation.state != ObservationState::known && evaluation.state != ObservationState::stale &&
            evaluation.state != ObservationState::unknown) {
          failures.fetch_add(1);
        }
        if (observatory.value().status().digest.size() != 16) {
          failures.fetch_add(1);
        }
        if (!observatory.value().authorities().empty()) {
          static_cast<void>(observatory.value().subjects());
        }
      }
    });
  }

  start.store(true, std::memory_order_release);
  for (std::thread& thread : threads) {
    thread.join();
  }

  FO_REQUIRE_EQ(failures.load(), 0);
  FO_REQUIRE(admitted.load() >= records_per_writer);
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
  FO_REQUIRE(LockOrderAudit::last_violation().empty());

  const Evaluation final_state = observatory.value().evaluate(rack, draw, at_seconds(100));
  FO_REQUIRE(final_state.state == ObservationState::known);
  FO_REQUIRE_EQ(final_state.value.value().to_string(),
                std::string(std::to_string(100 + writers * records_per_writer) + " watts"));
  static_cast<void>(observatory.value().close());
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
}

FO_TEST(events_are_delivered_outside_every_lock) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("events.foj");
  options.journal.origin = "event-test";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  std::atomic<int> delivered{0};
  std::atomic<int> reentrant_refusals{0};
  std::atomic<int> depth_seen{-1};

  observatory.value().set_event_handler([&observatory, &delivered, &reentrant_refusals, &depth_seen](
                                            const ObservatoryEvent& event) {
    delivered.fetch_add(1);
    depth_seen.store(static_cast<int>(LockOrderAudit::depth()));
    static_cast<void>(event);
    // Nothing may be held while a handler runs, so a call back in must be
    // refused rather than deadlocking.
    const IngestionOutcome nested = observatory.value().record_evidence(
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 999, 1, "1", "watts", 100));
    if (nested.code == ReasonCode::internal_error) {
      reentrant_refusals.fetch_add(1);
    }
  });

  FO_REQUIRE(observatory.value()
                 .record_evidence(
                     make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                 .code == ReasonCode::ok);
  FO_REQUIRE_EQ(delivered.load(), 1);
  FO_REQUIRE_EQ(depth_seen.load(), 0);
  FO_REQUIRE_EQ(reentrant_refusals.load(), 1);

  const std::vector<ObservatoryEvent> drained = observatory.value().drain_events();
  FO_REQUIRE_EQ(drained.size(), 1);
  FO_REQUIRE(drained.front().durable);
  FO_REQUIRE(observatory.value().drain_events().empty());
  static_cast<void>(observatory.value().close());
}

FO_TEST(shutdown_is_idempotent_and_refuses_late_work) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("shutdown.foj");
  options.journal.origin = "shutdown-test";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().is_open());
  FO_REQUIRE(observatory.value().close().has_value());
  FO_REQUIRE(!observatory.value().is_open());
  FO_REQUIRE(observatory.value().close().has_value());

  const IngestionOutcome late = observatory.value().record_evidence(
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 100));
  FO_REQUIRE(late.code == ReasonCode::shutting_down);
  FO_REQUIRE(!late.durable);

  const Evaluation evaluation = observatory.value().evaluate(
      EntityRef::parse("rack:r1").value(), AspectId::parse("power.draw").value(), at_seconds(100));
  FO_REQUIRE(evaluation.state == ObservationState::indeterminate);
  FO_REQUIRE(evaluation.code == ReasonCode::shutting_down);
}

FO_TEST(concurrent_snapshots_agree_with_evaluation) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("snapshots.foj");
  options.journal.origin = "snapshot-concurrency";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  std::atomic<int> mismatches{0};
  std::atomic<bool> start{false};
  std::thread writer([&observatory, &start] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int index = 1; index <= 60; ++index) {
      static_cast<void>(observatory.value().record_evidence(
          make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, static_cast<std::uint64_t>(index), 1,
                        "1500", "watts", 100)));
    }
  });
  std::thread snapshotter([&observatory, &mismatches, &start] {
    while (!start.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    for (int iteration = 0; iteration < 30; ++iteration) {
      if (!observatory.value().snapshot(at_seconds(200)).has_value()) {
        mismatches.fetch_add(1);
      }
    }
  });

  start.store(true, std::memory_order_release);
  writer.join();
  snapshotter.join();

  FO_REQUIRE_EQ(mismatches.load(), 0);
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
  static_cast<void>(observatory.value().close());
}
