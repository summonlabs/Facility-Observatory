// Facility Observatory - typed contracts with adjacent runtimes.
//
// The contracts here are deliberately narrow. They carry a publication from an
// adjacent runtime into the observatory and nothing else: there is no message
// that asks the observatory to change facility state, and any document that
// would require it is refused with an explicit reason.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_CONTRACTS_HPP
#define FACILITY_OBSERVATORY_CONTRACTS_HPP

#include <cstdint>
#include <string>
#include <string_view>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/json.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

// The only evidence contract version this build accepts or produces.
FO_API std::uint32_t evidence_contract_version() noexcept;

struct FO_API EvidenceEnvelope {
  std::uint32_t contract_version{1};
  AuthorityKind authority_kind{AuthorityKind::unknown};
  AuthorityId authority{};
  SourceId source{};
  Epoch epoch{};
  Generation generation{};
  Revision revision{};
  Timestamp observed_at{};
  Timestamp published_at{};
  EntityRef entity{};
  AspectId aspect{};
  Value value{};
  Durability durability{Durability::live};
  std::string origin{};
  std::string explanation{};

  friend bool operator==(const EvidenceEnvelope&, const EvidenceEnvelope&) = default;
};

// Which domains a runtime family is contractually permitted to publish into.
// This does not grant authority; it refuses to reinterpret one runtime's payload
// as a claim from another.
FO_API bool kind_may_publish_into(AuthorityKind kind, Domain domain) noexcept;

FO_API Status assert_envelope_boundary(const EvidenceEnvelope& envelope);

FO_API Outcome<EvidenceRecord> to_evidence(const EvidenceEnvelope& envelope, Timestamp received_at);

FO_API JsonValue envelope_to_json(const EvidenceEnvelope& envelope);
FO_API Outcome<EvidenceEnvelope> envelope_from_json(const JsonValue& document);

// ---------------------------------------------------------------------------
// Ingest document
//
// One JSON object per line. Three shapes are accepted, and nothing else.
// ---------------------------------------------------------------------------
struct FO_API IngestDocument {
  enum class Kind : std::uint8_t {
    authority = 0,
    epoch = 1,
    evidence = 2,
  };

  Kind kind{Kind::evidence};
  AuthorityDescriptor authority{};
  AuthorityId epoch_authority{};
  Epoch epoch{};
  Timestamp published_at{};
  EvidenceEnvelope envelope{};
};

FO_API std::string_view to_string(IngestDocument::Kind kind) noexcept;

FO_API JsonValue ingest_document_to_json(const IngestDocument& document);
FO_API Outcome<IngestDocument> ingest_document_from_json(const JsonValue& document);

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_CONTRACTS_HPP
