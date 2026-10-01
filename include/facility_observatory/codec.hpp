// Facility Observatory - explicit binary codec.
//
// Every decoded value is validated: lengths are bounded, identifiers are
// re-checked against their class, units must be known, and a trailing byte is a
// decoding error rather than a tolerated extra. Persistence therefore cannot
// smuggle an unvalidated value back into the runtime.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_CODEC_HPP
#define FACILITY_OBSERVATORY_CODEC_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/policy.hpp"

namespace fo {

class FO_API Encoder {
 public:
  void u8(std::uint8_t value);
  void u16(std::uint16_t value);
  void u32(std::uint32_t value);
  void u64(std::uint64_t value);
  void i64(std::int64_t value);
  void boolean(bool value);
  void sized_bytes(std::string_view value);
  void text(const std::string& value);
  void timestamp(const Timestamp& value);

  [[nodiscard]] const std::vector<std::uint8_t>& buffer() const noexcept { return buffer_; }
  [[nodiscard]] std::size_t size() const noexcept { return buffer_.size(); }

 private:
  std::vector<std::uint8_t> buffer_{};
};

class FO_API Decoder {
 public:
  explicit Decoder(std::span<const std::uint8_t> input) noexcept : input_(input) {}

  Outcome<std::uint8_t> u8();
  Outcome<std::uint16_t> u16();
  Outcome<std::uint32_t> u32();
  Outcome<std::uint64_t> u64();
  Outcome<std::int64_t> i64();
  Outcome<bool> boolean();
  Outcome<std::string_view> sized_bytes();
  Outcome<std::string> text();
  Outcome<Timestamp> timestamp();

  [[nodiscard]] bool at_end() const noexcept { return cursor_ == input_.size(); }
  [[nodiscard]] std::size_t remaining() const noexcept { return input_.size() - cursor_; }
  Status require_end() const;

 private:
  Outcome<std::span<const std::uint8_t>> take(std::size_t count);

  std::span<const std::uint8_t> input_{};
  std::size_t cursor_{0};
};

FO_API std::vector<std::uint8_t> encode_evidence(const EvidenceRecord& record);
FO_API Outcome<EvidenceRecord> decode_evidence(std::span<const std::uint8_t> bytes);

FO_API std::vector<std::uint8_t> encode_authority(const AuthorityDescriptor& descriptor);
FO_API Outcome<AuthorityDescriptor> decode_authority(std::span<const std::uint8_t> bytes);

FO_API std::vector<std::uint8_t> encode_epoch_advance(const AuthorityId& authority, Epoch epoch,
                                                      Timestamp published_at);
FO_API Outcome<Nothing> decode_epoch_advance(std::span<const std::uint8_t> bytes, AuthorityId& authority,
                                             Epoch& epoch, Timestamp& published_at);

FO_API std::vector<std::uint8_t> encode_policy(const AspectPolicy& policy);
FO_API Outcome<AspectPolicy> decode_policy(std::span<const std::uint8_t> bytes);

FO_API std::vector<std::uint8_t> encode_snapshot_marker(JournalSequence sequence, std::string_view digest,
                                                        Timestamp created_at);
FO_API Outcome<Nothing> decode_snapshot_marker(std::span<const std::uint8_t> bytes, JournalSequence& sequence,
                                               std::string& digest, Timestamp& created_at);

FO_API std::vector<std::uint8_t> encode_session_marker(std::string_view origin, Timestamp at);
FO_API Outcome<Nothing> decode_session_marker(std::span<const std::uint8_t> bytes, std::string& origin,
                                              Timestamp& at);

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_CODEC_HPP
