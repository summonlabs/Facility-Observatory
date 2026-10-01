// Facility Observatory - versioned, integrity-checked evidence journal.
//
// On-disk layout (all integers little-endian):
//
//   file header, 32 bytes
//     +0   magic           8 bytes  "FCOBJRN1"
//     +8   format_version  u32
//     +12  flags           u32      must be zero
//     +16  created_at      i64      unix nanoseconds
//     +24  header_crc32c   u32      CRC-32C over bytes [0, 24)
//     +28  reserved        u32      must be zero
//
//   frame, repeated
//     +0   payload_length  u32      must be <= max_payload_bytes
//     +4   record_type     u8
//     +5   flags           u8       must be zero
//     +6   reserved        u16      must be zero
//     +8   sequence        u64      strictly 1, 2, 3, ... with no gaps
//     +16  committed_at    i64      unix nanoseconds
//     +24  payload         payload_length bytes
//         crc32c           u32      CRC-32C over bytes [0, 24 + payload_length)
//
// The commit point is the successful return of Journal::commit(), which flushes
// every frame appended since the previous commit. Recovery reproduces exactly
// the frames whose commit point was reached and discards a torn tail.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_JOURNAL_HPP
#define FACILITY_OBSERVATORY_JOURNAL_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/codec.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/platform.hpp"

namespace fo {

enum class JournalRecordType : std::uint8_t {
  authority_registered = 1,
  epoch_advanced = 2,
  evidence = 3,
  policy_set = 4,
  snapshot_marker = 5,
  session_open = 6,
  session_close = 7,
};

FO_API std::string_view to_string(JournalRecordType type) noexcept;
FO_API Outcome<JournalRecordType> journal_record_type_from_string(std::string_view text);

struct FO_API JournalOptions {
  std::string path{};
  bool create_if_missing{true};
  // When a torn tail is found, truncate to the last complete frame. When false,
  // the journal refuses to open so an operator can inspect it first.
  bool truncate_torn_tail{true};
  // Make every append durable before returning. The commit point is still
  // explicit; this only decides whether it happens automatically.
  bool commit_on_append{true};
  std::uint64_t max_payload_bytes{1024ULL * 1024ULL};
  std::size_t max_records{1000000};
  std::string origin{"facility-observatory"};
  // Used only when the journal file is created. When unset, the system clock is
  // sampled once at creation time.
  Timestamp created_at{};
  // Opens the journal for reading only: no writer lock is taken, nothing is ever
  // written or truncated, and a torn tail is reported rather than repaired.
  bool read_only{false};
};

struct FO_API RecoveryReport {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  bool header_valid{false};
  bool clean{false};
  bool created{false};
  bool torn_tail{false};
  bool interior_corruption{false};
  bool truncated{false};
  std::uint64_t total_bytes{0};
  std::uint64_t valid_bytes{0};
  std::uint64_t lost_bytes{0};
  std::uint64_t record_count{0};
  std::uint32_t format_version{0};
  Timestamp created_at{};
};

struct FO_API JournalRecord {
  JournalRecordType type{JournalRecordType::evidence};
  JournalSequence sequence{};
  Timestamp committed_at{};
  std::vector<std::uint8_t> payload{};
};

struct FO_API AppendResult {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  bool committed{false};
  JournalSequence sequence{};
  std::uint64_t offset{0};
  std::uint64_t bytes{0};
};

// Read-only structural scan. Takes no lock and never modifies the file.
FO_API Outcome<RecoveryReport> inspect_journal(const std::string& path);

class FO_API Journal {
 public:
  Journal() = default;
  ~Journal();
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  Journal(Journal&& other) noexcept;
  Journal& operator=(Journal&& other) noexcept;

  // Takes the kernel-enforced single-writer lock, validates the header, scans
  // every frame and, when permitted, conservatively truncates a torn tail.
  static Outcome<Journal> open(const JournalOptions& options);

  [[nodiscard]] bool is_open() const noexcept { return file_.is_open(); }
  [[nodiscard]] bool is_read_only() const noexcept { return read_only_; }
  [[nodiscard]] const std::string& path() const noexcept { return file_.path(); }
  [[nodiscard]] const RecoveryReport& recovery() const noexcept { return recovery_; }
  [[nodiscard]] const std::vector<JournalRecord>& records() const noexcept { return records_; }
  [[nodiscard]] JournalSequence next_sequence() const noexcept { return next_sequence_; }
  [[nodiscard]] std::uint64_t committed_bytes() const noexcept { return valid_bytes_; }

  AppendResult append(JournalRecordType type, const std::vector<std::uint8_t>& payload, Timestamp at);
  AppendResult append_evidence(const EvidenceRecord& record, Timestamp at);
  AppendResult append_authority(const AuthorityDescriptor& descriptor, Timestamp at);
  AppendResult append_policy(const AspectPolicy& policy, Timestamp at);
  AppendResult append_snapshot_marker(JournalSequence sequence, std::string_view digest, Timestamp at);

  Outcome<Nothing> commit();

  void close() noexcept;

 private:
  AppendResult append_raw(JournalRecordType type, const std::uint8_t* payload, std::size_t size, Timestamp at);

  DurableFile file_{};
  FileLock lock_{};
  RecoveryReport recovery_{};
  std::vector<JournalRecord> records_{};
  JournalSequence next_sequence_{};
  std::uint64_t valid_bytes_{0};
  std::uint64_t max_payload_bytes_{1024ULL * 1024ULL};
  std::size_t max_records_{1000000};
  bool commit_on_append_{true};
  bool read_only_{false};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_JOURNAL_HPP
