// Facility Observatory - adversarial hardening tests.
//
// These target the ways the runtime is most likely to be wrong: paths beyond the
// classic platform limit, sources that go quiet, handlers that misbehave, and
// lifecycle calls that race the work they are shutting down.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "facility_observatory/lock_order.hpp"
#include "facility_observatory/observatory.hpp"
#include "facility_observatory/store.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

FO_TEST(long_journal_paths_beyond_the_classic_limit) {
  TempDir directory;
  std::string base = directory.path();
#if defined(_WIN32)
  for (char& c : base) {
    if (c == '/') {
      c = '\\';
    }
  }
  base = "\\\\?\\" + base;
#endif

#if defined(_WIN32)
  constexpr char kSeparator = '\\';
#else
  constexpr char kSeparator = '/';
#endif
  std::string current = base;
  int depth = 0;
  while (current.size() < 320 && depth < 40) {
    current.push_back(kSeparator);
    current.append("segment-");
    current.append(std::to_string(depth));
    current.append("-padding-padding-padding-padding");
    ++depth;
  }

  std::error_code error;
  std::filesystem::create_directories(std::filesystem::path(current), error);
  FO_REQUIRE_MESSAGE(!error, "cannot create the long directory chain: " + error.message());

  const std::string journal = current + kSeparator + "observatory.foj";
  FO_REQUIRE_MESSAGE(journal.size() > 260,
                     "the constructed path is only " + std::to_string(journal.size()) + " characters");

  {
    ObservatoryOptions options;
    options.journal.path = journal;
    options.journal.origin = "long-path";
    auto observatory = Observatory::open(options);
    FO_REQUIRE_MESSAGE(observatory.has_value(),
                       "cannot open a long journal path: " + to_debug_string(observatory.reason()));
    FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
    const IngestionOutcome outcome = observatory.value().record_evidence(
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100));
    FO_REQUIRE(outcome.code == ReasonCode::ok);
    FO_REQUIRE(outcome.durable);
    static_cast<void>(observatory.value().close());
  }

  {
    ObservatoryOptions options;
    options.journal.path = journal;
    options.journal.origin = "long-path-reopen";
    auto observatory = Observatory::open(options);
    FO_REQUIRE(observatory.has_value());
    FO_REQUIRE(observatory.value().status().recovery.clean);
    static_cast<void>(observatory.value().close());
  }
}

FO_TEST(source_disappearance_is_distinct_from_ageing) {
  EvidenceStore store;
  FO_REQUIRE(store.register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  // The authority spoke about draw long ago and about state just now.
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.state", 1, 1, 1, "1", "watts", 100000))
                  .retained);

  const Evaluation disappeared = store.evaluate(rack, draw, at_seconds(100001));
  FO_REQUIRE(disappeared.state == ObservationState::stale);
  FO_REQUIRE(disappeared.code == ReasonCode::source_disappeared);
  FO_REQUIRE(disappeared.detail.find("stopped publishing") != std::string::npos);
  FO_REQUIRE_EQ(disappeared.participant_notes.size(), 1);
  FO_REQUIRE(disappeared.participant_notes.front().has_value());

  // With no live aspect at all, the same silence reads as ordinary ageing.
  EvidenceStore quiet;
  FO_REQUIRE(quiet.register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  FO_REQUIRE(quiet.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                  .retained);
  const Evaluation aged = quiet.evaluate(rack, draw, at_seconds(100001));
  FO_REQUIRE(aged.state == ObservationState::stale);
  FO_REQUIRE(aged.code == ReasonCode::expired_evidence);
}

FO_TEST(a_throwing_event_handler_does_not_poison_the_thread) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("throwing.foj");
  options.journal.origin = "throwing-handler";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  std::atomic<int> calls{0};
  observatory.value().set_event_handler([&calls](const ObservatoryEvent&) {
    calls.fetch_add(1);
    throw std::runtime_error("handler failure");
  });

  bool threw = false;
  try {
    static_cast<void>(observatory.value().record_evidence(
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100)));
  } catch (const std::runtime_error&) {
    threw = true;
  }
  FO_REQUIRE_MESSAGE(threw, "the handler exception did not propagate");
  FO_REQUIRE_EQ(calls.load(), 1);

  // Clearing the handler is itself part of the contract under test: a removed
  // handler is never invoked again.
  observatory.value().set_event_handler(nullptr);

  // The dispatch flag must have been cleared by unwinding: a later call from the
  // same thread must not be mistaken for handler re-entrancy.
  const IngestionOutcome second = observatory.value().record_evidence(
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 2, 1, "1600", "watts", 100));
  FO_REQUIRE(second.code != ReasonCode::internal_error);
  FO_REQUIRE(second.code == ReasonCode::ok);
  static_cast<void>(observatory.value().close());
}

FO_TEST(an_event_handler_cannot_re_enter_the_observatory) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("reentrant.foj");
  options.journal.origin = "reentrant-handler";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  std::atomic<int> refusals{0};
  observatory.value().set_event_handler([&observatory, &refusals](const ObservatoryEvent&) {
    if (observatory.value().record_evidence(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 900, 1,
                                                          "1", "watts", 100))
            .code == ReasonCode::internal_error) {
      refusals.fetch_add(1);
    }
    if (observatory.value().snapshot(Timestamp{}).code() == ReasonCode::internal_error) {
      refusals.fetch_add(1);
    }
    if (observatory.value().evaluate(EntityRef::parse("rack:r1").value(), AspectId::parse("power.draw").value(),
                                     at_seconds(100))
            .code == ReasonCode::internal_error) {
      refusals.fetch_add(1);
    }
  });

  FO_REQUIRE(observatory.value()
                 .record_evidence(
                     make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                 .code == ReasonCode::ok);
  FO_REQUIRE_EQ(refusals.load(), 3);
  static_cast<void>(observatory.value().close());
}

FO_TEST(close_racing_ingestion_is_safe) {
  TempDir directory;
  LockOrderAudit::reset();
  ObservatoryOptions options;
  options.journal.path = directory.file("racing.foj");
  options.journal.origin = "racing-close";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());

  std::atomic<int> unexpected{0};
  std::atomic<int> completed{0};
  std::thread writer([&observatory, &unexpected, &completed] {
    for (int index = 1; index <= 400; ++index) {
      const IngestionOutcome outcome = observatory.value().record_evidence(
          make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, static_cast<std::uint64_t>(index), 1,
                        "1500", "watts", 100));
      const bool expected = outcome.code == ReasonCode::ok ||
                            outcome.code == ReasonCode::superseded_revision ||
                            outcome.code == ReasonCode::conflicting_evidence ||
                            outcome.code == ReasonCode::shutting_down ||
                            outcome.code == ReasonCode::cancelled;
      if (!expected) {
        unexpected.fetch_add(1);
      }
      completed.fetch_add(1);
    }
  });

  std::this_thread::sleep_for(std::chrono::milliseconds(15));
  FO_REQUIRE(observatory.value().close().has_value());
  writer.join();

  FO_REQUIRE_EQ(unexpected.load(), 0);
  FO_REQUIRE(completed.load() > 0);
  FO_REQUIRE_EQ(LockOrderAudit::violations(), 0);
}

FO_TEST(reconstruction_bounds_are_total) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("bounds.foj");
  options.journal.origin = "bounds";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  for (int index = 1; index <= 3; ++index) {
    FO_REQUIRE(observatory.value()
                   .record_evidence(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1,
                                                  static_cast<std::uint64_t>(index), 1, "1500", "watts", 100))
                   .durable);
  }

  auto everything = observatory.value().reconstruct(JournalSequence::from_value(1000000), at_seconds(200));
  FO_REQUIRE(everything.has_value());
  FO_REQUIRE_EQ(everything.value().digest, observatory.value().status().digest);

  auto nothing = observatory.value().reconstruct(JournalSequence::from_value(0), at_seconds(200));
  FO_REQUIRE(nothing.has_value());
  FO_REQUIRE_EQ(nothing.value().record_count, 0);
  FO_REQUIRE(nothing.value().digest != everything.value().digest);
  static_cast<void>(observatory.value().close());
}

FO_TEST(empty_journal_answers_without_inventing_anything) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("empty.foj");
  options.journal.origin = "empty";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE_EQ(observatory.value().status().records, 0);
  FO_REQUIRE_EQ(observatory.value().status().authorities, 0);
  FO_REQUIRE(observatory.value().subjects().empty());
  FO_REQUIRE(observatory.value().authorities().empty());

  const Evaluation evaluation = observatory.value().evaluate(
      EntityRef::parse("rack:nothing").value(), AspectId::parse("power.draw").value(), at_seconds(1));
  FO_REQUIRE(evaluation.state == ObservationState::unknown);
  FO_REQUIRE(evaluation.code == ReasonCode::no_evidence);
  FO_REQUIRE(evaluation.participants.empty());

  auto divergences = observatory.value().divergences(at_seconds(1));
  FO_REQUIRE(divergences.has_value());
  FO_REQUIRE(divergences.value().empty());

  const AggregateResult aggregate = observatory.value().aggregate(
      EntityRef::parse("site:nothing").value(), {}, AspectId::parse("power.draw").value(), Aggregation::sum, true,
      at_seconds(1));
  FO_REQUIRE(aggregate.state == ObservationState::unknown);
  FO_REQUIRE_EQ(aggregate.expected_contributors, 0);
  static_cast<void>(observatory.value().close());
}

FO_TEST(default_policy_survives_a_restart) {
  TempDir directory;
  const std::string journal = directory.file("policy.foj");

  {
    ObservatoryOptions options;
    options.journal.path = journal;
    options.journal.origin = "policy-a";
    auto observatory = Observatory::open(options);
    FO_REQUIRE(observatory.has_value());
    AspectPolicy fallback;
    fallback.freshness.fresh_for = std::chrono::seconds(11);
    fallback.freshness.stale_after = std::chrono::seconds(99);
    FO_REQUIRE(observatory.value().set_default_policy(fallback).has_value());

    AspectPolicy named = make_policy("power.draw", 3, 30);
    FO_REQUIRE(observatory.value().set_policy(named).has_value());
    static_cast<void>(observatory.value().close());
  }

  {
    ObservatoryOptions options;
    options.journal.path = journal;
    options.journal.origin = "policy-b";
    auto observatory = Observatory::open(options);
    FO_REQUIRE(observatory.has_value());
    const PolicySet policies = observatory.value().policy_set();
    FO_REQUIRE(policies.default_policy().freshness.fresh_for == std::chrono::seconds(11));
    FO_REQUIRE(policies.default_policy().freshness.stale_after == std::chrono::seconds(99));
    const AspectId draw = AspectId::parse("power.draw").value();
    FO_REQUIRE(policies.has_explicit_policy(draw));
    FO_REQUIRE(policies.policy_for(draw).freshness.fresh_for == std::chrono::seconds(3));
    static_cast<void>(observatory.value().close());
  }
}

FO_TEST(unauthorised_boundary_operations_stay_refused_everywhere) {
  for (const BoundaryOperation operation :
       {BoundaryOperation::command_facility_state, BoundaryOperation::set_policy,
        BoundaryOperation::transition_incident, BoundaryOperation::allocate_capacity,
        BoundaryOperation::place_workload, BoundaryOperation::schedule_maintenance,
        BoundaryOperation::control_power, BoundaryOperation::control_cooling, BoundaryOperation::drive_recovery,
        BoundaryOperation::mutate_identity, BoundaryOperation::publish_financial_state}) {
    const Status refused = Observatory::assert_operation(operation);
    FO_REQUIRE_MESSAGE(!refused.has_value(), std::string("accepted ") + std::string(to_string(operation)));
    FO_REQUIRE(refused.code() == ReasonCode::mutation_refused);
  }
  for (const BoundaryOperation operation :
       {BoundaryOperation::read_evidence, BoundaryOperation::record_evidence, BoundaryOperation::evaluate_view,
        BoundaryOperation::inspect_history}) {
    FO_REQUIRE_MESSAGE(Observatory::assert_operation(operation).has_value(),
                       std::string("refused ") + std::string(to_string(operation)));
  }
}
