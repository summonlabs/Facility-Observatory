// Facility Observatory - benchmarks.
//
// Every figure below counts completed work: the number of operations that ran to
// a successful return, divided by the wall time those operations took. Each
// figure is labelled REAL, SYNTHETIC or UNSUPPORTED, and the UNSUPPORTED list
// names the claims this repository deliberately does not make.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "facility_observatory/divergence.hpp"
#include "facility_observatory/json.hpp"
#include "facility_observatory/observatory.hpp"
#include "facility_observatory/store.hpp"

using namespace fo;

namespace {

constexpr std::uint64_t kDefaultScale = 1;

struct Measurement {
  std::string name{};
  std::string evidence_class{};
  std::uint64_t completed{0};
  double nanoseconds{0.0};
  std::string note{};
};

std::vector<Measurement>& measurements() {
  static std::vector<Measurement> list;
  return list;
}

void record(const std::string& name, const std::string& evidence_class, std::uint64_t completed,
            double nanoseconds, const std::string& note) {
  Measurement measurement;
  measurement.name = name;
  measurement.evidence_class = evidence_class;
  measurement.completed = completed;
  measurement.nanoseconds = nanoseconds;
  measurement.note = note;
  measurements().push_back(std::move(measurement));
}

Timestamp at_seconds(std::int64_t seconds) {
  return Timestamp::from_unix_nanos(seconds * 1000000000LL).value();
}

AuthorityDescriptor authority(const char* id, AuthorityKind kind) {
  AuthorityDescriptor descriptor;
  descriptor.id = AuthorityId::parse(id).value();
  descriptor.kind = kind;
  descriptor.role = AuthorityRole::authority;
  descriptor.epoch = Epoch::from_value(1);
  descriptor.epoch_published_at = at_seconds(0);
  return descriptor;
}

EvidenceRecord record_for(const char* authority_id, const std::string& subject, const char* aspect,
                          std::uint64_t generation, const char* decimal, const char* unit) {
  EvidenceRecord record;
  record.key.authority = AuthorityId::parse(authority_id).value();
  record.key.source = SourceId::parse("telemetry").value();
  record.key.entity = EntityRef::parse(subject).value();
  record.key.aspect = AspectId::parse(aspect).value();
  record.epoch = Epoch::from_value(1);
  record.generation = Generation::from_value(generation);
  record.revision = Revision::from_value(1);
  record.observed_at = at_seconds(100);
  record.published_at = at_seconds(100);
  record.value = Value::make_scalar(FixedPoint::parse(decimal).value(), unit_from_string(unit).value()).value();
  record.provenance.received_at = record.published_at;
  record.provenance.origin = "benchmark";
  return record;
}

std::string subject_name(std::uint64_t index) { return "rack:bench-" + std::to_string(index); }

void bench_journal_append(std::uint64_t count, const std::string& workspace) {
  const std::string path = workspace + "/append.foj";
  JournalOptions options;
  options.path = path;
  options.origin = "benchmark";
  options.created_at = at_seconds(0);
  auto journal = Journal::open(options);
  if (!journal) {
    std::fprintf(stderr, "benchmark: cannot open journal: %s\n", to_debug_string(journal.reason()).c_str());
    return;
  }
  static_cast<void>(journal.value().append_authority(authority("dccp-bench", AuthorityKind::dccp), at_seconds(0)));

  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  for (std::uint64_t index = 1; index <= count; ++index) {
    const EvidenceRecord item = record_for("dccp-bench", subject_name(index % 64), "power.draw", index, "1500", "watts");
    const AppendResult result = journal.value().append_evidence(item, at_seconds(100 + static_cast<std::int64_t>(index)));
    if (result.code == ReasonCode::ok && result.committed) {
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("journal_append_committed", "REAL", completed, elapsed,
         "one evidence frame appended and made durable (fsync) per operation, on the real file system");
  journal.value().close();
}

void bench_recovery(std::uint64_t count, const std::string& workspace) {
  const std::string path = workspace + "/recovery.foj";
  {
    JournalOptions options;
    options.path = path;
    options.origin = "benchmark-seed";
    options.created_at = at_seconds(0);
    auto journal = Journal::open(options);
    if (!journal) {
      return;
    }
    for (std::uint64_t index = 1; index <= count; ++index) {
      const EvidenceRecord item = record_for("dccp-bench", subject_name(index % 64), "power.draw", index, "1500", "watts");
      static_cast<void>(
          journal.value().append_evidence(item, at_seconds(100 + static_cast<std::int64_t>(index))));
    }
    journal.value().close();
  }

  const auto started = std::chrono::steady_clock::now();
  JournalOptions options;
  options.path = path;
  options.origin = "benchmark-recover";
  auto journal = Journal::open(options);
  std::uint64_t completed = 0;
  if (journal) {
    completed = journal.value().recovery().record_count;
    journal.value().close();
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("journal_recovery_scan", "REAL", completed, elapsed,
         "header validation, full frame scan and CRC verification of every record on reopen");
}

void bench_admission(std::uint64_t count) {
  EvidenceStore store;
  static_cast<void>(store.register_authority(authority("dccp-bench", AuthorityKind::dccp)));
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  for (std::uint64_t index = 1; index <= count; ++index) {
    const EvidenceRecord item = record_for("dccp-bench", subject_name(index % 1024), "power.draw", index, "1500", "watts");
    if (store.admit(item).admitted) {
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("evidence_admission", "REAL", completed, elapsed,
         "in-memory admission decisions over a synthetic evidence set (real indices, real fencing)");
}

void bench_evaluation(std::uint64_t aspects, std::uint64_t iterations) {
  EvidenceStore store;
  static_cast<void>(store.register_authority(authority("dccp-bench", AuthorityKind::dccp)));
  static_cast<void>(store.register_authority(authority("bms-bench", AuthorityKind::bms)));
  for (std::uint64_t index = 0; index < aspects; ++index) {
    const std::string aspect = "metric.m" + std::to_string(index);
    static_cast<void>(store.admit(record_for("dccp-bench", "rack:bench-0", aspect.c_str(), 1, "1500", "watts")));
  }
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  const EntityRef subject = EntityRef::parse("rack:bench-0").value();
  for (std::uint64_t index = 0; index < iterations; ++index) {
    const std::string aspect = "metric.m" + std::to_string(index % aspects);
    const Evaluation evaluation = store.evaluate(subject, AspectId::parse(aspect).value(), at_seconds(100));
    if (evaluation.state != ObservationState::unknown) {
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("aspect_evaluation", "REAL", completed, elapsed,
         "freshness classification, fencing and conflict detection for one aspect of one subject");
}

void bench_divergence(std::uint64_t racks, std::uint64_t iterations) {
  EvidenceStore store;
  static_cast<void>(store.register_authority(authority("dccp-bench", AuthorityKind::dccp)));
  static_cast<void>(store.register_authority(authority("bms-bench", AuthorityKind::bms)));
  EvidenceRecord total = record_for("dccp-bench", "site:bench", "power.total", 1, "3000", "watts");
  static_cast<void>(store.admit(total));
  for (std::uint64_t index = 0; index < racks; ++index) {
    EvidenceRecord containment;
    containment.key.authority = AuthorityId::parse("dccp-bench").value();
    containment.key.source = SourceId::parse("topology").value();
    containment.key.entity = EntityRef::parse(subject_name(index)).value();
    containment.key.aspect = AspectId::parse("topology.parent").value();
    containment.epoch = Epoch::from_value(1);
    containment.generation = Generation::from_value(1);
    containment.revision = Revision::from_value(1);
    containment.observed_at = at_seconds(100);
    containment.published_at = at_seconds(100);
    containment.value = Value::make_text("site:bench").value();
    containment.provenance.received_at = containment.published_at;
    static_cast<void>(store.admit(containment));
    static_cast<void>(store.admit(record_for("dccp-bench", subject_name(index), "power.draw", 1, "1500", "watts")));
  }
  const TopologyConventions conventions =
      TopologyConventions::make("topology.parent", "power.total", "power.draw").value();
  const DivergenceQuery query = DivergenceQuery::make_default().value();
  DivergenceDetector detector(store, conventions, query);

  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  for (std::uint64_t index = 0; index < iterations; ++index) {
    auto found = detector.detect(at_seconds(100));
    if (found.has_value()) {
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("divergence_full_scan", "REAL", completed, elapsed,
         "cross-domain divergence scan over the whole synthetic facility, including aggregate reconciliation");
}

void bench_aggregation(std::uint64_t racks, std::uint64_t iterations) {
  EvidenceStore store;
  static_cast<void>(store.register_authority(authority("dccp-bench", AuthorityKind::dccp)));
  std::vector<EntityRef> subjects;
  for (std::uint64_t index = 0; index < racks; ++index) {
    subjects.push_back(EntityRef::parse(subject_name(index)).value());
    static_cast<void>(store.admit(record_for("dccp-bench", subject_name(index), "power.draw", 1, "1500", "watts")));
  }
  const EntityRef scope = EntityRef::parse("site:bench").value();
  const AspectId aspect = AspectId::parse("power.draw").value();

  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  for (std::uint64_t index = 0; index < iterations; ++index) {
    const AggregateResult result =
        store.aggregate(scope, subjects, aspect, Aggregation::sum, true, at_seconds(100));
    if (result.state == ObservationState::known) {
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("aggregate_sum_complete", "REAL", completed, elapsed,
         "checked-decimal summation of every constituent with completeness enforcement");
}

void bench_json(std::uint64_t iterations) {
  const std::string document =
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":7,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r1\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"1500\",\"unit\":\"watts\"}}";
  const auto started = std::chrono::steady_clock::now();
  std::uint64_t completed = 0;
  for (std::uint64_t index = 0; index < iterations; ++index) {
    auto parsed = JsonValue::parse(document);
    if (parsed) {
      static_cast<void>(parsed.value().dump());
      ++completed;
    }
  }
  const auto finished = std::chrono::steady_clock::now();
  const double elapsed =
      static_cast<double>(std::chrono::duration_cast<std::chrono::nanoseconds>(finished - started).count());
  record("json_parse_and_dump", "REAL", completed, elapsed,
         "one evidence document parsed, validated and re-rendered");
}

const char* const kUnsupported[] = {
    "multi-node or clustered operation: this build was validated on a single host only",
    "real BMS, DCIM, plant or electrical telemetry: no physical facility hardware is attached",
    "real cooling, power train or generator measurement: no such instrumentation is present",
    "network partition, clock skew or byzantine source behaviour across hosts",
    "throughput or latency on storage other than the local file system used for the run",
};

}  // namespace

int main(int argc, char** argv) {
  const std::uint64_t scale = argc > 1 ? static_cast<std::uint64_t>(std::strtoull(argv[1], nullptr, 10)) : kDefaultScale;

  const std::filesystem::path workspace =
      std::filesystem::temp_directory_path() / ("fo-benchmark-" + std::to_string(static_cast<unsigned long long>(
                                                                   std::chrono::steady_clock::now().time_since_epoch().count())));
  std::error_code error;
  std::filesystem::create_directories(workspace, error);

  bench_journal_append(200 * scale, workspace.string());
  bench_recovery(200 * scale, workspace.string());
  bench_admission(50000 * scale);
  bench_evaluation(200, 20000 * scale);
  bench_divergence(50 * scale, 5 * scale);
  bench_aggregation(200, 2000 * scale);
  bench_json(20000 * scale);

  std::filesystem::remove_all(workspace, error);

  JsonValue document = JsonValue::make_object();
  document.set("scale", JsonValue::make_integer(static_cast<std::int64_t>(scale)));
  document.set("host", JsonValue::make_string("local single host"));
  JsonValue array = JsonValue::make_array();
  for (const Measurement& measurement : measurements()) {
    JsonValue entry = JsonValue::make_object();
    entry.set("name", JsonValue::make_string(measurement.name));
    entry.set("evidence_class", JsonValue::make_string(measurement.evidence_class));
    entry.set("completed_operations", JsonValue::make_integer(static_cast<std::int64_t>(measurement.completed)));
    entry.set("elapsed_nanoseconds", JsonValue::make_string(std::to_string(static_cast<std::int64_t>(measurement.nanoseconds))));
    if (measurement.completed > 0 && measurement.nanoseconds > 0.0) {
      const double per_operation = measurement.nanoseconds / static_cast<double>(measurement.completed);
      entry.set("nanoseconds_per_operation",
                JsonValue::make_string(std::to_string(static_cast<std::int64_t>(per_operation))));
      entry.set("operations_per_second",
                JsonValue::make_string(std::to_string(
                    static_cast<std::int64_t>(1000000000.0 / per_operation))));
    }
    entry.set("note", JsonValue::make_string(measurement.note));
    array.push_back(std::move(entry));
  }
  document.set("measurements", std::move(array));

  JsonValue unsupported = JsonValue::make_array();
  for (const char* statement : kUnsupported) {
    unsupported.push_back(JsonValue::make_string(statement));
  }
  document.set("unsupported", std::move(unsupported));

  std::printf("%s\n", document.dump(2).c_str());
  return 0;
}
