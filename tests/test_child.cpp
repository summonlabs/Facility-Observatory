// Facility Observatory - multiprocess test helper.
//
// A deliberately tiny program whose only job is to exercise durable ownership
// from a genuinely separate operating-system process.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "facility_observatory/observatory.hpp"

using namespace fo;

namespace {

constexpr int kExitLocked = 4;

AuthorityDescriptor dccp_authority() {
  AuthorityDescriptor descriptor;
  descriptor.id = AuthorityId::parse("dccp-child").value();
  descriptor.kind = AuthorityKind::dccp;
  descriptor.role = AuthorityRole::authority;
  descriptor.label = Token::parse("child").value();
  descriptor.epoch = Epoch::from_value(1);
  descriptor.epoch_published_at = SystemClock{}.now();
  return descriptor;
}

EvidenceRecord sample_record(int index) {
  EvidenceRecord record;
  record.key.authority = AuthorityId::parse("dccp-child").value();
  record.key.source = SourceId::parse("telemetry").value();
  record.key.entity = EntityRef::parse("rack:child-r1").value();
  record.key.aspect = AspectId::parse("power.draw").value();
  record.epoch = Epoch::from_value(1);
  record.generation = Generation::from_value(static_cast<std::uint64_t>(index));
  record.revision = Revision::from_value(1);
  const Timestamp stamp = SystemClock{}.now();
  record.observed_at = stamp;
  record.published_at = stamp;
  record.value = Value::make_scalar(FixedPoint::parse("1500").value(), Unit::watts).value();
  record.provenance.received_at = stamp;
  record.provenance.origin = "test-child";
  return record;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: test_child <hold|write|trylock> <journal> [count|milliseconds]\n");
    return 2;
  }
  const std::string command = argv[1];
  const std::string path = argv[2];

  ObservatoryOptions options;
  options.journal.path = path;
  options.journal.origin = "test-child";

  auto observatory = Observatory::open(options);
  if (!observatory) {
    std::printf("open failed: %s\n", to_debug_string(observatory.reason()).c_str());
    return observatory.code() == ReasonCode::journal_locked ? kExitLocked : 1;
  }

  int status = 0;
  if (command == "hold") {
    const int milliseconds = argc > 3 ? std::atoi(argv[3]) : 500;
    std::this_thread::sleep_for(std::chrono::milliseconds(milliseconds));
  } else if (command == "write") {
    const int count = argc > 3 ? std::atoi(argv[3]) : 3;
    if (!observatory.value().register_authority(dccp_authority()).has_value()) {
      status = 1;
    } else {
      for (int index = 1; index <= count && status == 0; ++index) {
        const IngestionOutcome outcome = observatory.value().record_evidence(sample_record(index));
        if (!outcome.durable) {
          std::printf("record %d was not durable: %s\n", index, to_debug_string(Reason{outcome.code, outcome.detail}).c_str());
          status = 1;
        }
      }
    }
  } else if (command != "trylock") {
    std::fprintf(stderr, "unknown command '%s'\n", command.c_str());
    status = 2;
  }

  static_cast<void>(observatory.value().close());
  return status;
}
