// Facility Observatory - versioned, integrity-checked evidence journal.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/journal.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "facility_observatory/checked.hpp"
#include "facility_observatory/integrity.hpp"
#include "facility_observatory/version.hpp"

namespace fo {
namespace {

constexpr std::array<std::uint8_t, 8> kMagic{{'F', 'C', 'O', 'B', 'J', 'R', 'N', '1'}};
constexpr std::size_t kHeaderBytes = 32;
constexpr std::size_t kPreambleBytes = 24;
constexpr std::size_t kTrailerBytes = 4;
constexpr std::uint64_t kMinimumHeaderTotal = kHeaderBytes;

std::uint32_t load_u32(const std::uint8_t* cursor) {
  return static_cast<std::uint32_t>(cursor[0]) | (static_cast<std::uint32_t>(cursor[1]) << 8) |
         (static_cast<std::uint32_t>(cursor[2]) << 16) | (static_cast<std::uint32_t>(cursor[3]) << 24);
}

std::uint64_t load_u64(const std::uint8_t* cursor) {
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(cursor[index]) << (8 * index);
  }
  return value;
}

void store_u32(std::uint8_t* cursor, std::uint32_t value) {
  for (int index = 0; index < 4; ++index) {
    cursor[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU);
  }
}

void store_u64(std::uint8_t* cursor, std::uint64_t value) {
  for (int index = 0; index < 8; ++index) {
    cursor[index] = static_cast<std::uint8_t>((value >> (8 * index)) & 0xFFU);
  }
}

bool is_known_record_type(std::uint8_t raw) {
  return raw >= static_cast<std::uint8_t>(JournalRecordType::authority_registered) &&
         raw <= static_cast<std::uint8_t>(JournalRecordType::session_close);
}

struct ScanOutcome {
  ReasonCode code{ReasonCode::ok};
  std::string detail{};
  RecoveryReport report{};
  std::vector<JournalRecord> records{};
  std::uint64_t valid_bytes{0};
  JournalSequence next_sequence{};
  bool acceptable{false};
};

std::array<std::uint8_t, kHeaderBytes> make_header(Timestamp created_at) {
  std::array<std::uint8_t, kHeaderBytes> header{};
  std::memcpy(header.data(), kMagic.data(), kMagic.size());
  store_u32(header.data() + 8, FO_JOURNAL_FORMAT_VERSION);
  store_u32(header.data() + 12, 0U);
  store_u64(header.data() + 16, static_cast<std::uint64_t>(created_at.is_set() ? created_at.unix_nanos() : 0));
  const std::uint32_t checksum = crc32c(header.data(), 24);
  store_u32(header.data() + 24, checksum);
  store_u32(header.data() + 28, 0U);
  return header;
}

ScanOutcome scan(const DurableFile& file, std::uint64_t max_payload_bytes, std::size_t max_records,
                 std::uint64_t limit_bytes) {
  ScanOutcome outcome;
  auto total = file.size();
  if (!total) {
    outcome.code = total.code();
    outcome.detail = total.reason().detail;
    return outcome;
  }
  const std::uint64_t size = total.value();
  outcome.report.total_bytes = size;

  if (size < kMinimumHeaderTotal) {
    outcome.code = ReasonCode::journal_header_corrupt;
    outcome.detail = "journal is " + std::to_string(size) + " bytes, shorter than the " +
                     std::to_string(kHeaderBytes) + "-byte header";
    outcome.report.code = outcome.code;
    outcome.report.detail = outcome.detail;
    return outcome;
  }

  std::array<std::uint8_t, kHeaderBytes> header{};
  auto header_read = file.read_at(0, header.data(), header.size());
  if (!header_read || header_read.value() != header.size()) {
    outcome.code = ReasonCode::journal_io_error;
    outcome.detail = "cannot read the journal header";
    return outcome;
  }
  if (std::memcmp(header.data(), kMagic.data(), kMagic.size()) != 0) {
    outcome.code = ReasonCode::journal_header_corrupt;
    outcome.detail = "journal magic does not match; this is not a Facility Observatory journal";
    outcome.report.code = outcome.code;
    outcome.report.detail = outcome.detail;
    return outcome;
  }
  const std::uint32_t format_version = load_u32(header.data() + 8);
  const std::uint32_t flags = load_u32(header.data() + 12);
  const std::uint32_t reserved = load_u32(header.data() + 28);
  const std::uint32_t recorded_crc = load_u32(header.data() + 24);
  const std::uint32_t computed_crc = crc32c(header.data(), 24);
  if (recorded_crc != computed_crc) {
    outcome.code = ReasonCode::journal_header_corrupt;
    outcome.detail = "journal header checksum mismatch";
    outcome.report.code = outcome.code;
    outcome.report.detail = outcome.detail;
    return outcome;
  }
  if (format_version != FO_JOURNAL_FORMAT_VERSION) {
    outcome.code = ReasonCode::journal_version_unsupported;
    outcome.detail = "journal format version " + std::to_string(format_version) + " is not supported by this build (" +
                     std::to_string(FO_JOURNAL_FORMAT_VERSION) + ")";
    outcome.report.code = outcome.code;
    outcome.report.detail = outcome.detail;
    return outcome;
  }
  if (flags != 0U || reserved != 0U) {
    outcome.code = ReasonCode::journal_header_corrupt;
    outcome.detail = "journal header carries non-zero flags or reserved fields";
    outcome.report.code = outcome.code;
    outcome.report.detail = outcome.detail;
    return outcome;
  }

  outcome.report.header_valid = true;
  outcome.report.format_version = format_version;
  const std::uint64_t created_raw = load_u64(header.data() + 16);
  auto created = Timestamp::from_unix_nanos(static_cast<std::int64_t>(created_raw));
  if (created) {
    outcome.report.created_at = created.value();
  }

  const std::uint64_t scan_limit = (limit_bytes == 0 || limit_bytes > size) ? size : limit_bytes;

  std::vector<std::uint8_t> frame_buffer;
  std::uint64_t offset = kHeaderBytes;
  JournalSequence expected = JournalSequence::from_value(1);
  bool fault = false;
  bool fault_is_tail = false;
  std::string fault_detail;
  ReasonCode fault_code = ReasonCode::ok;

  while (offset < scan_limit) {
    const std::uint64_t remaining = scan_limit - offset;
    if (remaining < kPreambleBytes + kTrailerBytes) {
      fault = true;
      fault_is_tail = true;
      fault_code = ReasonCode::journal_torn_tail;
      fault_detail = "a frame preamble is truncated at offset " + std::to_string(offset);
      break;
    }
    std::array<std::uint8_t, kPreambleBytes> preamble{};
    auto preamble_read = file.read_at(offset, preamble.data(), preamble.size());
    if (!preamble_read || preamble_read.value() != preamble.size()) {
      outcome.code = ReasonCode::journal_io_error;
      outcome.detail = "cannot read the frame preamble at offset " + std::to_string(offset);
      return outcome;
    }
    const std::uint32_t payload_length = load_u32(preamble.data());
    const std::uint8_t record_type = preamble[4];
    const std::uint8_t frame_flags = preamble[5];
    const std::uint16_t frame_reserved = static_cast<std::uint16_t>(preamble[6]) |
                                         static_cast<std::uint16_t>(static_cast<std::uint16_t>(preamble[7]) << 8);
    const std::uint64_t sequence_raw = load_u64(preamble.data() + 8);
    const std::int64_t committed_raw = static_cast<std::int64_t>(load_u64(preamble.data() + 16));

    std::uint64_t frame_total = 0;
    if (!checked_add_u64(static_cast<std::uint64_t>(payload_length), kPreambleBytes + kTrailerBytes, frame_total)) {
      fault = true;
      fault_is_tail = (remaining <= kPreambleBytes + kTrailerBytes);
      fault_code = ReasonCode::journal_interior_corruption;
      fault_detail = "frame length at offset " + std::to_string(offset) + " is not representable";
      break;
    }

    if (payload_length > max_payload_bytes) {
      fault = true;
      fault_is_tail = false;
      fault_code = ReasonCode::journal_interior_corruption;
      fault_detail = "frame at offset " + std::to_string(offset) + " declares a payload of " +
                     std::to_string(payload_length) + " bytes, beyond the bound of " +
                     std::to_string(max_payload_bytes);
      break;
    }
    if (!is_known_record_type(record_type) || frame_flags != 0U || frame_reserved != 0U) {
      fault = true;
      fault_is_tail = false;
      fault_code = ReasonCode::journal_interior_corruption;
      fault_detail = "frame at offset " + std::to_string(offset) +
                     " has a preamble that no writer of this format could have produced";
      break;
    }
    if (frame_total > remaining) {
      fault = true;
      fault_is_tail = true;
      fault_code = ReasonCode::journal_torn_tail;
      fault_detail = "frame at offset " + std::to_string(offset) + " extends past the end of the journal";
      break;
    }

    frame_buffer.resize(static_cast<std::size_t>(frame_total));
    auto frame_read = file.read_at(offset, frame_buffer.data(), frame_buffer.size());
    if (!frame_read || frame_read.value() != frame_buffer.size()) {
      outcome.code = ReasonCode::journal_io_error;
      outcome.detail = "cannot read the frame at offset " + std::to_string(offset);
      return outcome;
    }
    const std::size_t crc_offset = kPreambleBytes + static_cast<std::size_t>(payload_length);
    const std::uint32_t recorded = load_u32(frame_buffer.data() + crc_offset);
    const std::uint32_t computed = crc32c(frame_buffer.data(), crc_offset);
    if (recorded != computed) {
      fault = true;
      // A checksum failure in the final frame is indistinguishable from a torn
      // write, so it is treated conservatively as a tail. Anything earlier has a
      // complete frame after it and is therefore real corruption.
      const bool at_end = (offset + frame_total) >= scan_limit;
      fault_is_tail = at_end;
      fault_code = at_end ? ReasonCode::journal_torn_tail : ReasonCode::journal_interior_corruption;
      fault_detail = "frame checksum mismatch at offset " + std::to_string(offset);
      break;
    }
    if (sequence_raw != expected.value()) {
      fault = true;
      fault_is_tail = false;
      fault_code = ReasonCode::journal_interior_corruption;
      fault_detail = "frame at offset " + std::to_string(offset) + " carries sequence " +
                     std::to_string(sequence_raw) + " where " + std::to_string(expected.value()) + " was required";
      break;
    }

    JournalRecord record;
    record.type = static_cast<JournalRecordType>(record_type);
    record.sequence = JournalSequence::from_value(sequence_raw);
    auto committed = Timestamp::from_unix_nanos(committed_raw);
    if (committed) {
      record.committed_at = committed.value();
    }
    if (payload_length > 0) {
      record.payload.assign(frame_buffer.begin() + static_cast<std::ptrdiff_t>(kPreambleBytes),
                            frame_buffer.begin() + static_cast<std::ptrdiff_t>(crc_offset));
    }
    if (outcome.records.size() >= max_records) {
      outcome.code = ReasonCode::too_many_records;
      outcome.detail = "journal holds more than " + std::to_string(max_records) + " records";
      outcome.report.code = outcome.code;
      outcome.report.detail = outcome.detail;
      return outcome;
    }
    outcome.records.push_back(std::move(record));
    auto next = expected.next();
    if (!next) {
      outcome.code = ReasonCode::arithmetic_overflow;
      outcome.detail = "journal sequence counter exhausted";
      return outcome;
    }
    expected = next.value();
    offset += frame_total;
  }

  outcome.valid_bytes = offset > scan_limit ? scan_limit : offset;
  outcome.next_sequence = expected;
  outcome.report.record_count = outcome.records.size();
  outcome.report.valid_bytes = outcome.valid_bytes;
  outcome.report.lost_bytes = size > outcome.valid_bytes ? size - outcome.valid_bytes : 0;

  if (!fault) {
    outcome.report.clean = true;
    outcome.report.code = ReasonCode::ok;
    outcome.report.detail = "journal is structurally clean";
    outcome.acceptable = true;
    return outcome;
  }

  outcome.code = fault_code;
  outcome.detail = fault_detail;
  outcome.report.code = fault_code;
  outcome.report.detail = fault_detail;
  if (fault_is_tail) {
    outcome.report.torn_tail = true;
    outcome.acceptable = true;
  } else {
    outcome.report.interior_corruption = true;
    outcome.acceptable = false;
  }
  return outcome;
}

}  // namespace

std::string_view to_string(JournalRecordType type) noexcept {
  switch (type) {
    case JournalRecordType::authority_registered:
      return std::string_view{"authority_registered"};
    case JournalRecordType::epoch_advanced:
      return std::string_view{"epoch_advanced"};
    case JournalRecordType::evidence:
      return std::string_view{"evidence"};
    case JournalRecordType::policy_set:
      return std::string_view{"policy_set"};
    case JournalRecordType::snapshot_marker:
      return std::string_view{"snapshot_marker"};
    case JournalRecordType::session_open:
      return std::string_view{"session_open"};
    case JournalRecordType::session_close:
      return std::string_view{"session_close"};
  }
  return std::string_view{"unknown"};
}

Outcome<JournalRecordType> journal_record_type_from_string(std::string_view text) {
  const std::array<std::pair<std::string_view, JournalRecordType>, 7> table{{
      {"authority_registered", JournalRecordType::authority_registered},
      {"epoch_advanced", JournalRecordType::epoch_advanced},
      {"evidence", JournalRecordType::evidence},
      {"policy_set", JournalRecordType::policy_set},
      {"snapshot_marker", JournalRecordType::snapshot_marker},
      {"session_open", JournalRecordType::session_open},
      {"session_close", JournalRecordType::session_close},
  }};
  for (const auto& entry : table) {
    if (entry.first == text) {
      return Outcome<JournalRecordType>::ok(entry.second);
    }
  }
  return Outcome<JournalRecordType>::fail(ReasonCode::invalid_argument,
                                          "unknown journal record type '" + std::string(text) + "'");
}

Outcome<RecoveryReport> inspect_journal(const std::string& path) {
  auto file = DurableFile::open_read(path);
  if (!file) {
    RecoveryReport report;
    report.code = file.code();
    report.detail = file.reason().detail;
    return Outcome<RecoveryReport>::fail(file.reason());
  }
  auto size = file.value().size();
  if (!size) {
    RecoveryReport report;
    report.code = size.code();
    report.detail = size.reason().detail;
    return Outcome<RecoveryReport>::fail(size.reason());
  }
  if (size.value() == 0) {
    RecoveryReport report;
    report.code = ReasonCode::journal_header_corrupt;
    report.detail = "journal file is empty and has never been initialised";
    return Outcome<RecoveryReport>::ok(report);
  }
  ScanOutcome scanned = scan(file.value(), 1024ULL * 1024ULL, 1000000, 0);
  if (scanned.code == ReasonCode::journal_io_error) {
    return Outcome<RecoveryReport>::fail(scanned.code, scanned.detail);
  }
  if (scanned.code == ReasonCode::too_many_records) {
    return Outcome<RecoveryReport>::fail(scanned.code, scanned.detail);
  }
  return Outcome<RecoveryReport>::ok(std::move(scanned.report));
}

Journal::~Journal() { close(); }

Journal::Journal(Journal&& other) noexcept = default;
Journal& Journal::operator=(Journal&& other) noexcept = default;

Outcome<Journal> Journal::open(const JournalOptions& options) {
  if (options.path.empty()) {
    return Outcome<Journal>::fail(ReasonCode::journal_path_invalid, "journal path is empty");
  }
  auto normalized = normalize_path(options.path);
  if (!normalized) {
    return normalized.propagate<Journal>();
  }

  Journal journal;
  journal.max_payload_bytes_ = options.max_payload_bytes;
  journal.max_records_ = options.max_records;
  journal.commit_on_append_ = options.commit_on_append;
  journal.read_only_ = options.read_only;

  if (options.read_only) {
    // A read-only open never truncates and never repairs, whatever
    // truncate_torn_tail says: it reports and leaves the file alone.
    auto reader = DurableFile::open_read(normalized.value());
    if (!reader) {
      return reader.propagate<Journal>();
    }
    journal.file_ = std::move(reader).value();
    auto reader_size = journal.file_.size();
    if (!reader_size) {
      return reader_size.propagate<Journal>();
    }
    if (reader_size.value() == 0) {
      return Outcome<Journal>::fail(ReasonCode::journal_missing,
                                    "journal '" + normalized.value() + "' is empty and has never been initialised");
    }
    ScanOutcome scanned = scan(journal.file_, options.max_payload_bytes, options.max_records, 0);
    if (scanned.code == ReasonCode::journal_io_error || scanned.code == ReasonCode::too_many_records) {
      return Outcome<Journal>::fail(scanned.code, scanned.detail);
    }
    if (!scanned.acceptable) {
      journal.recovery_ = scanned.report;
      return Outcome<Journal>::fail(scanned.code, scanned.detail);
    }
    journal.recovery_ = scanned.report;
    journal.recovery_.truncated = false;
    journal.records_ = std::move(scanned.records);
    journal.valid_bytes_ = scanned.valid_bytes;
    journal.next_sequence_ = scanned.next_sequence;
    return Outcome<Journal>::ok(std::move(journal));
  }

  auto lock = FileLock::acquire_exclusive(normalized.value() + ".lock");
  if (!lock) {
    return lock.propagate<Journal>();
  }
  journal.lock_ = std::move(lock).value();

  auto file = DurableFile::open_append(normalized.value());
  if (!file) {
    return file.propagate<Journal>();
  }
  journal.file_ = std::move(file).value();

  auto size = journal.file_.size();
  if (!size) {
    return size.propagate<Journal>();
  }

  if (size.value() == 0) {
    if (!options.create_if_missing) {
      return Outcome<Journal>::fail(ReasonCode::journal_missing,
                                    "journal '" + normalized.value() + "' does not exist and creation is disabled");
    }
    const Timestamp created_at = options.created_at.is_set() ? options.created_at : SystemClock{}.now();
    const auto header = make_header(created_at);
    auto written = journal.file_.append(header.data(), header.size());
    if (!written) {
      return written.propagate<Journal>();
    }
    auto committed = journal.file_.commit();
    if (!committed) {
      return committed.propagate<Journal>();
    }
    journal.recovery_.created = true;
    journal.recovery_.header_valid = true;
    journal.recovery_.clean = true;
    journal.recovery_.code = ReasonCode::ok;
    journal.recovery_.detail = "journal created";
    journal.recovery_.format_version = FO_JOURNAL_FORMAT_VERSION;
    journal.recovery_.created_at = created_at;
    journal.recovery_.total_bytes = header.size();
    journal.recovery_.valid_bytes = header.size();
    journal.valid_bytes_ = header.size();
    journal.next_sequence_ = JournalSequence::from_value(1);
  } else {
    ScanOutcome scanned = scan(journal.file_, options.max_payload_bytes, options.max_records, 0);
    if (scanned.code == ReasonCode::journal_io_error || scanned.code == ReasonCode::too_many_records) {
      return Outcome<Journal>::fail(scanned.code, scanned.detail);
    }
    if (!scanned.acceptable) {
      journal.recovery_ = scanned.report;
      return Outcome<Journal>::fail(scanned.code, scanned.detail);
    }
    journal.recovery_ = scanned.report;
    journal.records_ = std::move(scanned.records);
    journal.valid_bytes_ = scanned.valid_bytes;
    journal.next_sequence_ = scanned.next_sequence;
    if (scanned.report.torn_tail && !options.truncate_torn_tail) {
      return Outcome<Journal>::fail(
          ReasonCode::journal_torn_tail,
          "journal '" + normalized.value() + "' has a torn tail at offset " +
              std::to_string(scanned.valid_bytes) + " and truncation is disabled");
    }
    if (scanned.report.torn_tail) {
      auto truncated = journal.file_.truncate(scanned.valid_bytes);
      if (!truncated) {
        return truncated.propagate<Journal>();
      }
      auto committed = journal.file_.commit();
      if (!committed) {
        return committed.propagate<Journal>();
      }
      journal.recovery_.truncated = true;
      journal.recovery_.code = ReasonCode::journal_torn_tail;
      journal.recovery_.detail = scanned.detail +
                                 "; truncated to the last complete frame at offset " +
                                 std::to_string(scanned.valid_bytes);
      journal.recovery_.total_bytes = scanned.report.total_bytes;
    }
  }

  return Outcome<Journal>::ok(std::move(journal));
}

AppendResult Journal::append(JournalRecordType type, const std::vector<std::uint8_t>& payload, Timestamp at) {
  return append_raw(type, payload.data(), payload.size(), at);
}

AppendResult Journal::append_evidence(const EvidenceRecord& record, Timestamp at) {
  const std::vector<std::uint8_t> payload = encode_evidence(record);
  return append_raw(JournalRecordType::evidence, payload.data(), payload.size(), at);
}

AppendResult Journal::append_authority(const AuthorityDescriptor& descriptor, Timestamp at) {
  const std::vector<std::uint8_t> payload = encode_authority(descriptor);
  return append_raw(JournalRecordType::authority_registered, payload.data(), payload.size(), at);
}

AppendResult Journal::append_policy(const AspectPolicy& policy, Timestamp at) {
  const std::vector<std::uint8_t> payload = encode_policy(policy);
  return append_raw(JournalRecordType::policy_set, payload.data(), payload.size(), at);
}

AppendResult Journal::append_snapshot_marker(JournalSequence sequence, std::string_view digest, Timestamp at) {
  const std::vector<std::uint8_t> payload = encode_snapshot_marker(sequence, digest, at);
  return append_raw(JournalRecordType::snapshot_marker, payload.data(), payload.size(), at);
}

AppendResult Journal::append_raw(JournalRecordType type, const std::uint8_t* payload, std::size_t size,
                                 Timestamp at) {
  AppendResult result;
  if (!is_open()) {
    result.code = ReasonCode::journal_not_open;
    result.detail = "journal is not open";
    return result;
  }
  if (read_only_) {
    result.code = ReasonCode::mutation_refused;
    result.detail = "the journal was opened read-only and refuses every append";
    return result;
  }
  if (!at.is_set()) {
    result.code = ReasonCode::indeterminate;
    result.detail = "a journal append requires a set commit instant";
    return result;
  }
  if (size > max_payload_bytes_) {
    result.code = ReasonCode::record_too_large;
    result.detail = "payload of " + std::to_string(size) + " bytes exceeds the bound of " +
                    std::to_string(max_payload_bytes_) + " bytes";
    return result;
  }
  if (records_.size() >= max_records_) {
    result.code = ReasonCode::too_many_records;
    result.detail = "journal is at its bound of " + std::to_string(max_records_) + " records";
    return result;
  }
  auto next = next_sequence_.next();
  if (!next) {
    result.code = ReasonCode::arithmetic_overflow;
    result.detail = "journal sequence counter exhausted";
    return result;
  }

  const std::uint64_t offset = valid_bytes_;
  std::vector<std::uint8_t> frame(kPreambleBytes + size + kTrailerBytes, 0U);
  store_u32(frame.data(), static_cast<std::uint32_t>(size));
  frame[4] = static_cast<std::uint8_t>(type);
  frame[5] = 0U;
  frame[6] = 0U;
  frame[7] = 0U;
  store_u64(frame.data() + 8, next_sequence_.value());
  store_u64(frame.data() + 16, static_cast<std::uint64_t>(at.unix_nanos()));
  if (size > 0) {
    std::memcpy(frame.data() + kPreambleBytes, payload, size);
  }
  const std::size_t crc_offset = kPreambleBytes + size;
  store_u32(frame.data() + crc_offset, crc32c(frame.data(), crc_offset));

  auto written = file_.append(frame.data(), frame.size());
  if (!written) {
    result.code = written.code();
    result.detail = written.reason().detail;
    return result;
  }
  if (commit_on_append_) {
    auto committed = file_.commit();
    if (!committed) {
      result.code = ReasonCode::journal_commit_failed;
      result.detail = committed.reason().detail;
      return result;
    }
  }

  JournalRecord record;
  record.type = type;
  record.sequence = next_sequence_;
  record.committed_at = at;
  if (size > 0) {
    record.payload.assign(payload, payload + size);
  }
  records_.push_back(std::move(record));
  valid_bytes_ += frame.size();

  result.code = ReasonCode::ok;
  result.committed = commit_on_append_;
  result.sequence = next_sequence_;
  result.offset = offset;
  result.bytes = frame.size();
  next_sequence_ = next.value();
  return result;
}

Outcome<Nothing> Journal::commit() {
  if (!is_open()) {
    return Outcome<Nothing>::fail(ReasonCode::journal_not_open, "journal is not open");
  }
  if (read_only_) {
    return Outcome<Nothing>::fail(ReasonCode::mutation_refused,
                                  "the journal was opened read-only and has no commit point");
  }
  return file_.commit();
}

void Journal::close() noexcept {
  if (file_.is_open()) {
    file_.close();
  }
  if (lock_.held()) {
    lock_.release();
  }
}

}  // namespace fo
