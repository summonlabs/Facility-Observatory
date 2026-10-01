// Facility Observatory - evidence model.
//
// An evidence record is what an authority published, about which subject, at
// which position in that authority's own epoch/generation/revision sequence.
// Nothing here mutates the subject; it only records what was claimed.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_EVIDENCE_HPP
#define FACILITY_OBSERVATORY_EVIDENCE_HPP

#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/strong.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

// ---------------------------------------------------------------------------
// Domains and subjects
// ---------------------------------------------------------------------------
enum class Domain : std::uint8_t {
  facility = 0,
  site = 1,
  rack = 2,
  asset = 3,
  incident = 4,
  capacity = 5,
  dependency = 6,
  unknown = 255,
};

FO_API std::string_view to_string(Domain domain) noexcept;
FO_API Outcome<Domain> domain_from_string(std::string_view text);

// Observable subject. Domain is part of the identity so that a rack named "r07"
// and a site named "r07" can never be confused.
struct FO_API EntityRef {
  Domain domain{Domain::unknown};
  EntityId id{};

  static Outcome<EntityRef> parse(std::string_view text);

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const EntityRef&, const EntityRef&) = default;
  friend auto operator<=>(const EntityRef&, const EntityRef&) = default;
};

// ---------------------------------------------------------------------------
// Values
// ---------------------------------------------------------------------------
enum class ValueKind : std::uint8_t {
  absent = 0,
  scalar = 1,
  enumeration = 2,
  text = 3,
  boolean = 4,
};

FO_API std::string_view to_string(ValueKind kind) noexcept;

class FO_API Value {
 public:
  static constexpr std::size_t kMaxTextBytes = 256;

  Value() = default;

  static Value absent();
  static Outcome<Value> make_scalar(const FixedPoint& magnitude, Unit unit);
  static Outcome<Value> make_enumeration(const Token& token);
  static Outcome<Value> make_text(std::string_view text);
  static Outcome<Value> make_boolean(bool flag);

  [[nodiscard]] ValueKind kind() const noexcept { return kind_; }
  [[nodiscard]] bool is_absent() const noexcept { return kind_ == ValueKind::absent; }
  [[nodiscard]] bool has_scalar() const noexcept { return kind_ == ValueKind::scalar; }

  [[nodiscard]] const FixedPoint& scalar_magnitude() const noexcept { return scalar_; }
  [[nodiscard]] Unit unit() const noexcept { return unit_; }
  [[nodiscard]] const Token& enumeration_token() const noexcept { return enumeration_; }
  [[nodiscard]] const std::string& text_body() const noexcept { return text_; }
  [[nodiscard]] bool boolean_flag() const noexcept { return boolean_; }

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const Value& lhs, const Value& rhs) noexcept;
  friend bool operator!=(const Value& lhs, const Value& rhs) noexcept { return !(lhs == rhs); }

 private:
  ValueKind kind_{ValueKind::absent};
  FixedPoint scalar_{};
  Unit unit_{Unit::none};
  Token enumeration_{};
  std::string text_{};
  bool boolean_{false};
};

// ---------------------------------------------------------------------------
// Provenance and durability
// ---------------------------------------------------------------------------
enum class Durability : std::uint8_t {
  live = 0,       // accepted directly from the publishing authority
  recovered = 1,  // reconstructed from durable storage after a restart
  synthetic = 2,  // modelled; the publishing authority declared it modelled
  imported = 3,   // loaded from an offline evidence file
};

FO_API std::string_view to_string(Durability durability) noexcept;
FO_API Outcome<Durability> durability_from_string(std::string_view text);

struct FO_API Provenance {
  Timestamp received_at{};
  Durability durability{Durability::live};
  JournalSequence sequence{};
  Epoch epoch_at_receipt{};
  std::string origin{};

  friend bool operator==(const Provenance&, const Provenance&) = default;
};

// ---------------------------------------------------------------------------
// Evidence identity and record
// ---------------------------------------------------------------------------
struct FO_API EvidenceKey {
  AuthorityId authority{};
  SourceId source{};
  EntityRef entity{};
  AspectId aspect{};

  friend bool operator==(const EvidenceKey&, const EvidenceKey&) = default;
  friend auto operator<=>(const EvidenceKey&, const EvidenceKey&) = default;
};

struct FO_API EvidenceRecord {
  EvidenceKey key{};
  Epoch epoch{};
  Generation generation{};
  Revision revision{};
  Timestamp observed_at{};   // when the authority says it observed the world
  Timestamp published_at{};  // when the authority says it published this
  Value value{};
  Provenance provenance{};
  std::string explanation{};

  friend bool operator==(const EvidenceRecord&, const EvidenceRecord&) = default;
};

// Two records are the same publication when everything an authority controls is
// identical. Explanation text and receipt metadata are excluded: they describe
// our handling, not the claim.
FO_API bool same_publication(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept;

// Scale-insensitive value equality: 1.5 kW and 1500 W are equivalent, 1.5 kW
// and 1.4 kW are not. Values of different kinds are never equivalent.
FO_API bool value_equivalent(const Value& lhs, const Value& rhs);

// Total canonical order used for every deterministic output.
FO_API bool canonical_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept;

// Compact, stable dependency reference: enough to name exactly which evidence
// and which generation a result depends on, and where it sits durably.
struct FO_API EvidenceRef {
  EvidenceKey key{};
  Epoch epoch{};
  Generation generation{};
  Revision revision{};
  Timestamp observed_at{};
  Timestamp published_at{};
  Durability durability{Durability::live};
  JournalSequence sequence{};

  [[nodiscard]] std::string to_string() const;

  friend bool operator==(const EvidenceRef&, const EvidenceRef&) = default;
  friend auto operator<=>(const EvidenceRef&, const EvidenceRef&) = default;
};

FO_API EvidenceRef to_reference(const EvidenceRecord& record);

// ---------------------------------------------------------------------------
// Observation state
//
// Six states, all explicit. There is no "probably fine" state.
// ---------------------------------------------------------------------------
enum class ObservationState : std::uint8_t {
  known = 0,          // fresh, unambiguous, single answer
  stale = 1,          // evidence exists but is older than the freshness window
  unknown = 2,        // no evidence has been published for this subject
  conflicting = 3,    // two or more authorities, or one authority twice, disagree
  unsupported = 4,    // evidence exists but this runtime cannot interpret it
  indeterminate = 5,  // evidence exists but is not evaluable (future-dated, unset time)
};

FO_API std::string_view to_string(ObservationState state) noexcept;
FO_API Outcome<ObservationState> observation_state_from_string(std::string_view text);

// A value together with the evidence that asserted it.
struct FO_API ConflictingValue {
  Value value{};
  EvidenceRef source{};

  friend bool operator==(const ConflictingValue&, const ConflictingValue&) = default;
};

// ---------------------------------------------------------------------------
// Evaluation
//
// The complete, deterministic answer to "what is known about this subject and
// aspect right now, and exactly why".
// ---------------------------------------------------------------------------
struct FO_API Evaluation {
  EntityRef entity{};
  AspectId aspect{};
  ObservationState state{ObservationState::unknown};
  ReasonCode code{ReasonCode::no_evidence};
  std::string detail{};

  // Present only when state == known. Scalars are reported in the aspect's
  // canonical unit.
  std::optional<Value> value{};

  // Every record considered, in canonical order. This is the dependency set.
  std::vector<EvidenceRef> participants{};
  // The subset that produced the answer, or that is in disagreement.
  std::vector<EvidenceRef> contributors{};
  // Populated when state == conflicting; every distinct claim is preserved.
  std::vector<ConflictingValue> disagreement{};
  // Per-participant rejection reason, aligned with participants by index.
  std::vector<std::optional<Reason>> participant_notes{};

  [[nodiscard]] std::size_t considered_count() const noexcept { return participants.size(); }
};

// A flat, addressable record of everything the store knows about one subject.
struct FO_API SubjectRecord {
  EntityRef entity{};
  std::vector<AspectId> aspects{};
  friend bool operator==(const SubjectRecord&, const SubjectRecord&) = default;
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_EVIDENCE_HPP
