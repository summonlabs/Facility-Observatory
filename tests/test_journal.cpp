// Facility Observatory - journal, recovery and restart tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "facility_observatory/integrity.hpp"
#include "facility_observatory/journal.hpp"
#include "facility_observatory/observatory.hpp"
#include "facility_observatory/platform.hpp"
#include "facility_observatory/version.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kPreambleBytes = 24;
constexpr std::size_t kTrailerBytes = 4;

std::uint32_t load_u32(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return static_cast<std::uint32_t>(bytes[offset]) | (static_cast<std::uint32_t>(bytes[offset + 1]) << 8) |
         (static_cast<std::uint32_t>(bytes[offset + 2]) << 16) |
         (static_cast<std::uint32_t>(bytes[offset + 3]) << 24);
}

void store_u32(std::vector<std::uint8_t>& bytes, std::size_t offset, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    bytes[offset + static_cast<std::size_t>(index)] =
        static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU);
  }
}

std::vector<std::uint8_t> read_bytes(const std::string& path) {
  auto content = read_whole_file(path, 64U << 20);
  FO_REQUIRE(content.has_value());
  return content.value();
}

void write_bytes(const std::string& path, const std::vector<std::uint8_t>& bytes) {
  FO_REQUIRE(publish_atomically(path, bytes.data(), bytes.size()).has_value());
}

std::size_t frame_total(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  return kPreambleBytes + static_cast<std::size_t>(load_u32(bytes, offset)) + kTrailerBytes;
}

void build_journal(const std::string& path, int count) {
  JournalOptions options;
  options.path = path;
  options.origin = "journal-test";
  auto journal = Journal::open(options);
  FO_REQUIRE(journal.has_value());
  FO_REQUIRE(journal.value().append_authority(make_authority("dccp-a", AuthorityKind::dccp), at_seconds(1)).code ==
             ReasonCode::ok);
  for (int index = 1; index <= count; ++index) {
    const std::string load = std::to_string(index);
    const EvidenceRecord record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1,
                                                static_cast<std::uint64_t>(index), 1, load.c_str(), "watts", 100);
    FO_REQUIRE(journal.value().append_evidence(record, at_seconds(100 + index)).code == ReasonCode::ok);
  }
  journal.value().close();
}

}  // namespace

FO_TEST(journal_creates_and_reopens_cleanly) {
  TempDir directory;
  const std::string path = directory.file("clean.foj");
  FO_REQUIRE(!path_exists(path));

  {
    JournalOptions options;
    options.path = path;
    options.created_at = at_seconds(1000);
    auto journal = Journal::open(options);
    FO_REQUIRE(journal.has_value());
    FO_REQUIRE(journal.value().recovery().created);
    FO_REQUIRE(journal.value().recovery().clean);
    FO_REQUIRE(journal.value().recovery().header_valid);
    FO_REQUIRE_EQ(journal.value().recovery().format_version, journal_format_version());
    FO_REQUIRE_EQ(journal.value().next_sequence().value(), 1);
    FO_REQUIRE_EQ(
        journal.value().append_authority(make_authority("dccp-a", AuthorityKind::dccp), at_seconds(1)).sequence.value(),
        1);
    FO_REQUIRE_EQ(journal.value()
                      .append_evidence(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1",
                                                     "watts", 2),
                                       at_seconds(2))
                      .sequence.value(),
                  2);
    journal.value().close();
  }

  {
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(journal.has_value());
    FO_REQUIRE(!journal.value().recovery().created);
    FO_REQUIRE(journal.value().recovery().clean);
    FO_REQUIRE_EQ(journal.value().recovery().record_count, 2);
    FO_REQUIRE_EQ(journal.value().records().size(), 2);
    FO_REQUIRE(journal.value().records()[0].type == JournalRecordType::authority_registered);
    FO_REQUIRE(journal.value().records()[1].type == JournalRecordType::evidence);
    FO_REQUIRE_EQ(journal.value().next_sequence().value(), 3);

    auto decoded = decode_evidence(journal.value().records()[1].payload);
    FO_REQUIRE(decoded.has_value());
    FO_REQUIRE_EQ(decoded.value().key.entity.to_string(), std::string("rack:r1"));
    journal.value().close();
  }
}

FO_TEST(journal_refuses_concurrent_writers) {
  TempDir directory;
  const std::string path = directory.file("locked.foj");
  JournalOptions options;
  options.path = path;
  auto first = Journal::open(options);
  FO_REQUIRE(first.has_value());
  auto second = Journal::open(options);
  FO_REQUIRE(!second.has_value());
  FO_REQUIRE(second.code() == ReasonCode::journal_locked);
  first.value().close();

  auto third = Journal::open(options);
  FO_REQUIRE(third.has_value());
  third.value().close();
}

FO_TEST(journal_read_only_mode_never_writes) {
  TempDir directory;
  const std::string path = directory.file("readonly.foj");
  build_journal(path, 2);
  const std::vector<std::uint8_t> before = read_bytes(path);

  JournalOptions options;
  options.path = path;
  options.read_only = true;
  auto journal = Journal::open(options);
  FO_REQUIRE(journal.has_value());
  FO_REQUIRE(journal.value().is_read_only());
  FO_REQUIRE_EQ(journal.value().records().size(), 3);
  FO_REQUIRE(journal.value().append(JournalRecordType::evidence, std::vector<std::uint8_t>{}, at_seconds(5)).code ==
             ReasonCode::mutation_refused);
  FO_REQUIRE(!journal.value().commit().has_value());
  journal.value().close();

  FO_REQUIRE(read_bytes(path) == before);

  JournalOptions writer_options;
  writer_options.path = path;
  auto writer = Journal::open(writer_options);
  FO_REQUIRE(writer.has_value());
  JournalOptions reader_options;
  reader_options.path = path;
  reader_options.read_only = true;
  auto reader = Journal::open(reader_options);
  FO_REQUIRE(reader.has_value());
  reader.value().close();
  writer.value().close();
}

FO_TEST(journal_rejects_missing_and_short_files) {
  TempDir directory;
  FO_REQUIRE(!Journal::open(JournalOptions{}).has_value());

  const std::string path = directory.file("short.foj");
  write_bytes(path, std::vector<std::uint8_t>(16, 0U));
  JournalOptions options;
  options.path = path;
  auto short_file = Journal::open(options);
  FO_REQUIRE(!short_file.has_value());
  FO_REQUIRE(short_file.code() == ReasonCode::journal_header_corrupt);

  auto report = inspect_journal(path);
  FO_REQUIRE(report.has_value());
  FO_REQUIRE(report.value().code == ReasonCode::journal_header_corrupt);

  auto missing = inspect_journal(directory.file("absent.foj"));
  FO_REQUIRE(!missing.has_value());
  FO_REQUIRE(missing.code() == ReasonCode::journal_missing);
}

FO_TEST(journal_rejects_header_damage) {
  TempDir directory;
  const std::string path = directory.file("header.foj");
  build_journal(path, 1);
  const std::vector<std::uint8_t> clean = read_bytes(path);

  {
    std::vector<std::uint8_t> damaged = clean;
    damaged[0] = static_cast<std::uint8_t>('X');
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_header_corrupt);
  }
  {
    std::vector<std::uint8_t> damaged = clean;
    store_u32(damaged, 8, 99U);
    damaged[24] = static_cast<std::uint8_t>(damaged[24] ^ 0xFFU);
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_header_corrupt);
  }
  {
    std::vector<std::uint8_t> damaged = clean;
    store_u32(damaged, 8, 99U);
    store_u32(damaged, 24, crc32c(damaged.data(), 24));
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_version_unsupported);
  }
  {
    std::vector<std::uint8_t> damaged = clean;
    store_u32(damaged, 12, 1U);
    store_u32(damaged, 24, crc32c(damaged.data(), 24));
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_header_corrupt);
  }
}

FO_TEST(journal_recovers_a_torn_tail_conservatively) {
  TempDir directory;
  const std::string path = directory.file("torn.foj");
  build_journal(path, 3);
  const std::vector<std::uint8_t> clean = read_bytes(path);

  const std::size_t cuts[] = {3U, 9U, 21U};
  for (const std::size_t cut : cuts) {
    std::vector<std::uint8_t> torn(clean.begin(), clean.end() - static_cast<std::ptrdiff_t>(cut));
    write_bytes(path, torn);

    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(journal.has_value());
    FO_REQUIRE(journal.value().recovery().torn_tail);
    FO_REQUIRE(journal.value().recovery().truncated);
    FO_REQUIRE(!journal.value().recovery().interior_corruption);
    FO_REQUIRE_EQ(journal.value().records().size(), 3);
    journal.value().close();

    auto again = Journal::open(options);
    FO_REQUIRE(again.has_value());
    FO_REQUIRE(again.value().recovery().clean);
    FO_REQUIRE_EQ(again.value().records().size(), 3);
    again.value().close();
  }
}

FO_TEST(journal_refuses_to_truncate_when_told_not_to) {
  TempDir directory;
  const std::string path = directory.file("nocommit.foj");
  build_journal(path, 2);
  const std::vector<std::uint8_t> clean = read_bytes(path);
  std::vector<std::uint8_t> torn(clean.begin(), clean.end() - 5);
  write_bytes(path, torn);

  JournalOptions options;
  options.path = path;
  options.truncate_torn_tail = false;
  auto journal = Journal::open(options);
  FO_REQUIRE(!journal.has_value());
  FO_REQUIRE(journal.code() == ReasonCode::journal_torn_tail);
  FO_REQUIRE(read_bytes(path) == torn);
}

FO_TEST(journal_rejects_interior_corruption) {
  TempDir directory;
  const std::string path = directory.file("interior.foj");
  build_journal(path, 4);
  const std::vector<std::uint8_t> clean = read_bytes(path);

  const std::size_t first = frame_total(clean, kHeaderBytes);
  const std::size_t second_offset = kHeaderBytes + first;
  const std::size_t second = frame_total(clean, second_offset);
  FO_REQUIRE(second_offset + second < clean.size());

  std::vector<std::uint8_t> damaged = clean;
  damaged[second_offset + kPreambleBytes + 1] =
      static_cast<std::uint8_t>(damaged[second_offset + kPreambleBytes + 1] ^ 0xFFU);
  write_bytes(path, damaged);

  JournalOptions options;
  options.path = path;
  auto journal = Journal::open(options);
  FO_REQUIRE(!journal.has_value());
  FO_REQUIRE(journal.code() == ReasonCode::journal_interior_corruption);

  auto report = inspect_journal(path);
  FO_REQUIRE(report.has_value());
  FO_REQUIRE(report.value().interior_corruption);
  FO_REQUIRE(!report.value().torn_tail);
  FO_REQUIRE(read_bytes(path) == damaged);
}

FO_TEST(journal_detects_a_sequence_gap) {
  TempDir directory;
  const std::string path = directory.file("gap.foj");
  build_journal(path, 3);
  const std::vector<std::uint8_t> clean = read_bytes(path);

  const std::size_t first = frame_total(clean, kHeaderBytes);
  const std::size_t second = frame_total(clean, kHeaderBytes + first);
  std::vector<std::uint8_t> gapped;
  gapped.insert(gapped.end(), clean.begin(), clean.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + first));
  gapped.insert(gapped.end(), clean.begin() + static_cast<std::ptrdiff_t>(kHeaderBytes + first + second), clean.end());
  write_bytes(path, gapped);

  JournalOptions options;
  options.path = path;
  auto journal = Journal::open(options);
  FO_REQUIRE(!journal.has_value());
  FO_REQUIRE(journal.code() == ReasonCode::journal_interior_corruption);
}

FO_TEST(journal_rejects_impossible_frame_shapes) {
  TempDir directory;
  const std::string path = directory.file("shape.foj");
  build_journal(path, 3);
  const std::vector<std::uint8_t> clean = read_bytes(path);
  const std::size_t first = frame_total(clean, kHeaderBytes);

  {
    std::vector<std::uint8_t> damaged = clean;
    store_u32(damaged, kHeaderBytes, 0xFFFFFFF0U);
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_interior_corruption);
  }
  {
    std::vector<std::uint8_t> damaged = clean;
    damaged[kHeaderBytes + 4] = 200U;
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_interior_corruption);
  }
  {
    std::vector<std::uint8_t> damaged = clean;
    const std::size_t second_offset = kHeaderBytes + first;
    FO_REQUIRE(second_offset + kPreambleBytes < damaged.size());
    damaged[second_offset + 5] = 1U;
    write_bytes(path, damaged);
    JournalOptions options;
    options.path = path;
    auto journal = Journal::open(options);
    FO_REQUIRE(!journal.has_value());
    FO_REQUIRE(journal.code() == ReasonCode::journal_interior_corruption);
  }
}

FO_TEST(journal_payload_limits_are_enforced) {
  TempDir directory;
  const std::string path = directory.file("limits.foj");
  JournalOptions options;
  options.path = path;
  options.max_payload_bytes = 64;
  auto journal = Journal::open(options);
  FO_REQUIRE(journal.has_value());
  const std::vector<std::uint8_t> oversized(128, 0U);
  const AppendResult refused = journal.value().append(JournalRecordType::evidence, oversized, at_seconds(1));
  FO_REQUIRE(refused.code == ReasonCode::record_too_large);
  FO_REQUIRE(!refused.committed);
  FO_REQUIRE(journal.value().append(JournalRecordType::evidence, std::vector<std::uint8_t>{}, Timestamp{}).code ==
             ReasonCode::indeterminate);
  journal.value().close();

  JournalOptions record_limit;
  record_limit.path = directory.file("recordlimit.foj");
  record_limit.max_records = 2;
  auto limited = Journal::open(record_limit);
  FO_REQUIRE(limited.has_value());
  FO_REQUIRE(limited.value().append(JournalRecordType::evidence, std::vector<std::uint8_t>{1U}, at_seconds(1)).code ==
             ReasonCode::ok);
  FO_REQUIRE(limited.value().append(JournalRecordType::evidence, std::vector<std::uint8_t>{2U}, at_seconds(2)).code ==
             ReasonCode::ok);
  FO_REQUIRE(limited.value().append(JournalRecordType::evidence, std::vector<std::uint8_t>{3U}, at_seconds(3)).code ==
             ReasonCode::too_many_records);
  limited.value().close();
}

FO_TEST(observatory_restart_is_durable_and_recovered) {
  TempDir directory;
  const std::string path = directory.file("restart.foj");
  const EntityRef rack = EntityRef::parse("rack:r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();

  std::string first_digest;
  {
    ObservatoryOptions options;
    options.journal.path = path;
    options.journal.origin = "restart-a";
    auto observatory = Observatory::open(options);
    FO_REQUIRE(observatory.has_value());
    FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
    const IngestionOutcome outcome = observatory.value().record_evidence(
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100));
    FO_REQUIRE(outcome.code == ReasonCode::ok);
    FO_REQUIRE(outcome.durable);
    FO_REQUIRE(outcome.sequence.value() >= 1);
    FO_REQUIRE(observatory.value().evaluate(rack, draw, at_seconds(100)).state == ObservationState::known);
    first_digest = observatory.value().status().digest;
    static_cast<void>(observatory.value().close());
  }

  {
    ObservatoryOptions options;
    options.journal.path = path;
    options.journal.origin = "restart-b";
    auto observatory = Observatory::open(options);
    FO_REQUIRE(observatory.has_value());
    FO_REQUIRE_EQ(observatory.value().status().records, 1);
    FO_REQUIRE(observatory.value().knows_subject(rack));

    const Evaluation recovered = observatory.value().evaluate(rack, draw, at_seconds(100));
    FO_REQUIRE(recovered.state == ObservationState::stale);
    FO_REQUIRE(recovered.code == ReasonCode::recovered_not_current);
    FO_REQUIRE_EQ(observatory.value().status().digest, first_digest);

    EvidenceRecord refresh =
        make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 2, 1, "1500", "watts", 100);
    FO_REQUIRE(observatory.value().record_evidence(refresh).code == ReasonCode::ok);
    const Evaluation current = observatory.value().evaluate(rack, draw, at_seconds(100));
    FO_REQUIRE(current.state == ObservationState::known);
    FO_REQUIRE_EQ(current.value.value().to_string(), std::string("1500 watts"));
    static_cast<void>(observatory.value().close());
  }
}

FO_TEST(observatory_snapshot_and_reconstruction_agree) {
  TempDir directory;
  ObservatoryOptions options;
  options.journal.path = directory.file("snapshot.foj");
  options.journal.origin = "snapshot-test";
  auto observatory = Observatory::open(options);
  FO_REQUIRE(observatory.has_value());
  FO_REQUIRE(observatory.value().register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  for (int index = 1; index <= 5; ++index) {
    const std::string load = std::to_string(index * 100);
    FO_REQUIRE(observatory.value()
                   .record_evidence(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1,
                                                  static_cast<std::uint64_t>(index), 1, load.c_str(), "watts", 100))
                   .durable);
  }

  auto snapshot = observatory.value().snapshot(at_seconds(200));
  FO_REQUIRE(snapshot.has_value());
  const std::string live = observatory.value().status().digest;
  FO_REQUIRE_EQ(snapshot.value().digest, live);

  auto rebuilt = observatory.value().reconstruct(snapshot.value().sequence, at_seconds(200));
  FO_REQUIRE(rebuilt.has_value());
  FO_REQUIRE_EQ(rebuilt.value().digest, live);
  FO_REQUIRE_EQ(rebuilt.value().record_count, snapshot.value().record_count);

  auto earlier = observatory.value().reconstruct(JournalSequence::from_value(3), at_seconds(200));
  FO_REQUIRE(earlier.has_value());
  FO_REQUIRE(earlier.value().record_count < rebuilt.value().record_count);
  static_cast<void>(observatory.value().close());
}

FO_TEST(observatory_refuses_interior_corruption_on_open) {
  TempDir directory;
  const std::string path = directory.file("corrupt-open.foj");
  build_journal(path, 4);
  std::vector<std::uint8_t> damaged = read_bytes(path);
  const std::size_t first = frame_total(damaged, kHeaderBytes);
  const std::size_t second_offset = kHeaderBytes + first;
  damaged[second_offset + kPreambleBytes] = static_cast<std::uint8_t>(damaged[second_offset + kPreambleBytes] ^ 0x5AU);
  write_bytes(path, damaged);

  ObservatoryOptions options;
  options.journal.path = path;
  auto observatory = Observatory::open(options);
  FO_REQUIRE(!observatory.has_value());
  FO_REQUIRE(observatory.code() == ReasonCode::journal_interior_corruption);
}
