// Facility Observatory - independent downstream consumer.
//
// This program only ever sees the public headers and the installed library. It
// exercises the parts of the contract a downstream DCCP, ASI or DFI component
// would rely on: recording an observation, reading back a deterministic
// evaluation with its reasons, and reading an explicit boundary refusal.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdio>
#include <string>

#include <facility_observatory/observatory.hpp>
#include <facility_observatory/render.hpp>

namespace {

int fail(const std::string& message) {
  std::fprintf(stderr, "consumer: %s\n", message.c_str());
  return 1;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: observatory_consumer <journal-path>\n");
    return 2;
  }

  fo::ObservatoryOptions options;
  options.journal.path = argv[1];
  options.journal.origin = "consumer";

  auto observatory = fo::Observatory::open(options);
  if (!observatory) {
    return fail("cannot open the journal: " + fo::to_debug_string(observatory.reason()));
  }

  // The boundary is part of the contract: a consumer cannot make the observatory
  // take authority it does not own.
  const fo::Status refused = fo::Observatory::assert_operation(fo::BoundaryOperation::control_cooling);
  if (refused) {
    return fail("the observatory accepted an operation it does not own");
  }

  fo::AuthorityDescriptor authority;
  authority.id = fo::AuthorityId::parse("dccp-consumer").value();
  authority.kind = fo::AuthorityKind::dccp;
  authority.role = fo::AuthorityRole::authority;
  authority.epoch = fo::Epoch::from_value(1);
  authority.epoch_published_at = fo::SystemClock{}.now();
  if (!observatory.value().register_authority(authority)) {
    return fail("cannot register the authority");
  }

  const fo::Timestamp observed = fo::SystemClock{}.now();
  fo::EvidenceRecord record;
  record.key.authority = authority.id;
  record.key.source = fo::SourceId::parse("telemetry").value();
  record.key.entity = fo::EntityRef::parse("rack:consumer-r1").value();
  record.key.aspect = fo::AspectId::parse("power.draw").value();
  record.epoch = fo::Epoch::from_value(1);
  record.generation = fo::Generation::from_value(1);
  record.revision = fo::Revision::from_value(1);
  record.observed_at = observed;
  record.published_at = observed;
  record.value = fo::Value::make_scalar(fo::FixedPoint::parse("1.5").value(), fo::Unit::kilowatts).value();
  record.provenance.received_at = observed;
  record.provenance.origin = "consumer";

  const fo::IngestionOutcome ingested = observatory.value().record_evidence(record);
  if (!ingested.durable || ingested.code != fo::ReasonCode::ok) {
    return fail("the observation was not made durable: " +
                fo::to_debug_string(fo::Reason{ingested.code, ingested.detail}));
  }

  const fo::Evaluation evaluation = observatory.value().evaluate(record.key.entity, record.key.aspect, observed);
  if (evaluation.state != fo::ObservationState::known) {
    return fail("expected a known state, got " + std::string(fo::to_string(evaluation.state)));
  }
  std::printf("%s\n", fo::render_evaluation(evaluation).dump().c_str());

  const fo::Status closed = observatory.value().close();
  if (!closed) {
    return fail("cannot close the observatory cleanly");
  }
  return 0;
}
