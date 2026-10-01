// Facility Observatory - typed contracts with adjacent runtimes.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/contracts.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <utility>

namespace fo {
namespace {

struct DomainPermission {
  AuthorityKind kind;
  std::array<Domain, 6> domains;
  std::size_t count;
};

constexpr std::array<DomainPermission, 9> kPermissions{{
    {AuthorityKind::dccp, {Domain::facility, Domain::site, Domain::rack, Domain::capacity, Domain::incident, Domain::unknown}, 5},
    {AuthorityKind::asi, {Domain::asset, Domain::dependency, Domain::rack, Domain::site, Domain::unknown, Domain::unknown}, 4},
    {AuthorityKind::dfi, {Domain::dependency, Domain::capacity, Domain::site, Domain::unknown, Domain::unknown, Domain::unknown}, 3},
    {AuthorityKind::bms, {Domain::facility, Domain::site, Domain::rack, Domain::unknown, Domain::unknown, Domain::unknown}, 3},
    {AuthorityKind::dcim, {Domain::facility, Domain::site, Domain::rack, Domain::asset, Domain::capacity, Domain::unknown}, 5},
    {AuthorityKind::plant, {Domain::facility, Domain::site, Domain::rack, Domain::asset, Domain::unknown, Domain::unknown}, 4},
    {AuthorityKind::economic, {Domain::capacity, Domain::site, Domain::unknown, Domain::unknown, Domain::unknown, Domain::unknown}, 2},
    {AuthorityKind::synthetic, {Domain::facility, Domain::site, Domain::rack, Domain::asset, Domain::incident, Domain::capacity}, 6},
    {AuthorityKind::unknown, {Domain::unknown, Domain::unknown, Domain::unknown, Domain::unknown, Domain::unknown, Domain::unknown}, 0},
}};

const JsonValue* member(const JsonValue& object, std::string_view key) {
  if (!object.is_object()) {
    return nullptr;
  }
  return object.find(key);
}

Outcome<std::string> required_string(const JsonValue& object, std::string_view key) {
  const JsonValue* value = member(object, key);
  if (value == nullptr) {
    return Outcome<std::string>::fail(ReasonCode::empty_input,
                                      "document is missing the required field '" + std::string(key) + "'");
  }
  if (!value->is_string()) {
    return Outcome<std::string>::fail(ReasonCode::malformed_encoding,
                                      "field '" + std::string(key) + "' must be a string");
  }
  return Outcome<std::string>::ok(std::string(value->string_or(std::string_view{})));
}

Outcome<std::uint64_t> required_counter(const JsonValue& object, std::string_view key) {
  const JsonValue* value = member(object, key);
  if (value == nullptr) {
    return Outcome<std::uint64_t>::fail(ReasonCode::empty_input,
                                        "document is missing the required field '" + std::string(key) + "'");
  }
  const std::int64_t raw = value->integer_or(-1);
  if (raw < 1) {
    return Outcome<std::uint64_t>::fail(ReasonCode::invalid_argument,
                                        "field '" + std::string(key) + "' must be an integer of at least 1");
  }
  return Outcome<std::uint64_t>::ok(static_cast<std::uint64_t>(raw));
}

JsonValue value_to_json(const Value& value) {
  JsonValue object = JsonValue::make_object();
  object.set("kind", JsonValue::make_string(std::string(to_string(value.kind()))));
  switch (value.kind()) {
    case ValueKind::absent:
      break;
    case ValueKind::scalar:
      object.set("decimal", JsonValue::make_string(value.scalar_magnitude().to_string()));
      object.set("unit", JsonValue::make_string(std::string(to_string(value.unit()))));
      break;
    case ValueKind::enumeration:
      object.set("token", JsonValue::make_string(value.enumeration_token().str()));
      break;
    case ValueKind::text:
      object.set("text", JsonValue::make_string(value.text_body()));
      break;
    case ValueKind::boolean:
      object.set("value", JsonValue::make_boolean(value.boolean_flag()));
      break;
  }
  return object;
}

Outcome<Value> value_from_json(const JsonValue& document) {
  if (!document.is_object()) {
    return Outcome<Value>::fail(ReasonCode::malformed_encoding, "'value' must be an object");
  }
  const JsonValue* kind = document.find("kind");
  if (kind == nullptr || !kind->is_string()) {
    return Outcome<Value>::fail(ReasonCode::empty_input, "'value.kind' is required and must be a string");
  }
  const std::string_view name = kind->string_or(std::string_view{});
  if (name == "absent") {
    return Outcome<Value>::ok(Value::absent());
  }
  if (name == "scalar") {
    auto decimal = required_string(document, "decimal");
    if (!decimal) {
      return decimal.propagate<Value>();
    }
    auto unit_text = required_string(document, "unit");
    if (!unit_text) {
      return unit_text.propagate<Value>();
    }
    auto magnitude = FixedPoint::parse(decimal.value());
    if (!magnitude) {
      return magnitude.propagate<Value>();
    }
    auto unit = unit_from_string(unit_text.value());
    if (!unit) {
      return unit.propagate<Value>();
    }
    return Value::make_scalar(magnitude.value(), unit.value());
  }
  if (name == "enumeration") {
    auto token_text = required_string(document, "token");
    if (!token_text) {
      return token_text.propagate<Value>();
    }
    auto token = Token::parse(token_text.value());
    if (!token) {
      return token.propagate<Value>();
    }
    return Value::make_enumeration(token.value());
  }
  if (name == "text") {
    auto body = required_string(document, "text");
    if (!body) {
      return body.propagate<Value>();
    }
    return Value::make_text(body.value());
  }
  if (name == "boolean") {
    const JsonValue* flag = document.find("value");
    if (flag == nullptr) {
      return Outcome<Value>::fail(ReasonCode::empty_input, "'value.value' is required for a boolean");
    }
    return Value::make_boolean(flag->boolean_or(false));
  }
  return Outcome<Value>::fail(ReasonCode::malformed_encoding,
                              "unknown value kind '" + std::string(name) + "'");
}

}  // namespace

std::uint32_t evidence_contract_version() noexcept { return 1U; }

bool kind_may_publish_into(AuthorityKind kind, Domain domain) noexcept {
  for (const DomainPermission& permission : kPermissions) {
    if (permission.kind != kind) {
      continue;
    }
    for (std::size_t index = 0; index < permission.count; ++index) {
      if (permission.domains[index] == domain) {
        return true;
      }
    }
    return false;
  }
  return false;
}

Status assert_envelope_boundary(const EvidenceEnvelope& envelope) {
  if (envelope.contract_version != evidence_contract_version()) {
    return Status::fail(ReasonCode::not_supported,
                        "evidence contract version " + std::to_string(envelope.contract_version) +
                            " is not supported by this build (" +
                            std::to_string(evidence_contract_version()) + ")");
  }
  if (envelope.authority_kind == AuthorityKind::unknown) {
    return Status::fail(ReasonCode::authority_unknown,
                        "an evidence envelope must declare the runtime family that published it");
  }
  if (!kind_may_publish_into(envelope.authority_kind, envelope.entity.domain)) {
    return Status::fail(ReasonCode::authority_boundary_violation,
                        "authority kind '" + std::string(to_string(envelope.authority_kind)) +
                            "' does not publish into domain '" + std::string(to_string(envelope.entity.domain)) +
                            "'");
  }
  if (envelope.epoch.is_unset() || envelope.generation.is_unset() || envelope.revision.is_unset()) {
    return Status::fail(ReasonCode::invalid_argument,
                        "epoch, generation and revision must all be at least 1");
  }
  if (!envelope.observed_at.is_set() || !envelope.published_at.is_set()) {
    return Status::fail(ReasonCode::indeterminate,
                        "observation and publication instants must both be present");
  }
  if (envelope.published_at < envelope.observed_at) {
    return Status::fail(ReasonCode::indeterminate, "publication instant precedes observation instant");
  }
  return success();
}

Outcome<EvidenceRecord> to_evidence(const EvidenceEnvelope& envelope, Timestamp received_at) {
  auto boundary = assert_envelope_boundary(envelope);
  if (!boundary) {
    return boundary.propagate<EvidenceRecord>();
  }
  EvidenceRecord record;
  record.key.authority = envelope.authority;
  record.key.source = envelope.source;
  record.key.entity = envelope.entity;
  record.key.aspect = envelope.aspect;
  record.epoch = envelope.epoch;
  record.generation = envelope.generation;
  record.revision = envelope.revision;
  record.observed_at = envelope.observed_at;
  record.published_at = envelope.published_at;
  record.value = envelope.value;
  record.provenance.received_at = received_at.is_set() ? received_at : envelope.published_at;
  record.provenance.durability = envelope.durability;
  record.provenance.epoch_at_receipt = envelope.epoch;
  record.provenance.origin = envelope.origin.empty() ? std::string("contract") : envelope.origin;
  record.explanation = envelope.explanation;
  return Outcome<EvidenceRecord>::ok(std::move(record));
}

JsonValue envelope_to_json(const EvidenceEnvelope& envelope) {
  JsonValue object = JsonValue::make_object();
  object.set("kind", JsonValue::make_string("evidence"));
  object.set("contract_version", JsonValue::make_integer(static_cast<std::int64_t>(envelope.contract_version)));
  object.set("authority", JsonValue::make_string(envelope.authority.str()));
  object.set("authority_kind", JsonValue::make_string(std::string(to_string(envelope.authority_kind))));
  object.set("source", JsonValue::make_string(envelope.source.str()));
  object.set("epoch", JsonValue::make_integer(static_cast<std::int64_t>(envelope.epoch.value())));
  object.set("generation", JsonValue::make_integer(static_cast<std::int64_t>(envelope.generation.value())));
  object.set("revision", JsonValue::make_integer(static_cast<std::int64_t>(envelope.revision.value())));
  object.set("observed_at", JsonValue::make_string(envelope.observed_at.to_rfc3339()));
  object.set("published_at", JsonValue::make_string(envelope.published_at.to_rfc3339()));
  object.set("subject", JsonValue::make_string(envelope.entity.to_string()));
  object.set("aspect", JsonValue::make_string(envelope.aspect.str()));
  object.set("value", value_to_json(envelope.value));
  object.set("durability", JsonValue::make_string(std::string(to_string(envelope.durability))));
  object.set("origin", JsonValue::make_string(envelope.origin));
  object.set("explanation", JsonValue::make_string(envelope.explanation));
  return object;
}

Outcome<EvidenceEnvelope> envelope_from_json(const JsonValue& document) {
  if (!document.is_object()) {
    return Outcome<EvidenceEnvelope>::fail(ReasonCode::malformed_encoding, "an evidence document must be an object");
  }
  EvidenceEnvelope envelope;
  envelope.contract_version = static_cast<std::uint32_t>(
      member(document, "contract_version") != nullptr
          ? member(document, "contract_version")->integer_or(static_cast<std::int64_t>(evidence_contract_version()))
          : static_cast<std::int64_t>(evidence_contract_version()));

  auto authority = required_string(document, "authority");
  if (!authority) {
    return authority.propagate<EvidenceEnvelope>();
  }
  auto authority_id = AuthorityId::parse(authority.value());
  if (!authority_id) {
    return authority_id.propagate<EvidenceEnvelope>();
  }
  envelope.authority = authority_id.value();

  auto kind = required_string(document, "authority_kind");
  if (!kind) {
    return kind.propagate<EvidenceEnvelope>();
  }
  auto authority_kind = authority_kind_from_string(kind.value());
  if (!authority_kind) {
    return authority_kind.propagate<EvidenceEnvelope>();
  }
  envelope.authority_kind = authority_kind.value();

  auto source = required_string(document, "source");
  if (!source) {
    return source.propagate<EvidenceEnvelope>();
  }
  auto source_id = SourceId::parse(source.value());
  if (!source_id) {
    return source_id.propagate<EvidenceEnvelope>();
  }
  envelope.source = source_id.value();

  auto epoch = required_counter(document, "epoch");
  if (!epoch) {
    return epoch.propagate<EvidenceEnvelope>();
  }
  envelope.epoch = Epoch::from_value(epoch.value());

  auto generation = required_counter(document, "generation");
  if (!generation) {
    return generation.propagate<EvidenceEnvelope>();
  }
  envelope.generation = Generation::from_value(generation.value());

  auto revision = required_counter(document, "revision");
  if (!revision) {
    return revision.propagate<EvidenceEnvelope>();
  }
  envelope.revision = Revision::from_value(revision.value());

  auto observed = required_string(document, "observed_at");
  if (!observed) {
    return observed.propagate<EvidenceEnvelope>();
  }
  auto observed_at = Timestamp::parse_rfc3339(observed.value());
  if (!observed_at) {
    return observed_at.propagate<EvidenceEnvelope>();
  }
  envelope.observed_at = observed_at.value();

  auto published = required_string(document, "published_at");
  if (!published) {
    return published.propagate<EvidenceEnvelope>();
  }
  auto published_at = Timestamp::parse_rfc3339(published.value());
  if (!published_at) {
    return published_at.propagate<EvidenceEnvelope>();
  }
  envelope.published_at = published_at.value();

  auto subject = required_string(document, "subject");
  if (!subject) {
    return subject.propagate<EvidenceEnvelope>();
  }
  auto entity = EntityRef::parse(subject.value());
  if (!entity) {
    return entity.propagate<EvidenceEnvelope>();
  }
  envelope.entity = entity.value();

  auto aspect = required_string(document, "aspect");
  if (!aspect) {
    return aspect.propagate<EvidenceEnvelope>();
  }
  auto aspect_id = AspectId::parse(aspect.value());
  if (!aspect_id) {
    return aspect_id.propagate<EvidenceEnvelope>();
  }
  envelope.aspect = aspect_id.value();

  const JsonValue* value = member(document, "value");
  if (value == nullptr) {
    return Outcome<EvidenceEnvelope>::fail(ReasonCode::empty_input, "'value' is required");
  }
  auto decoded_value = value_from_json(*value);
  if (!decoded_value) {
    return decoded_value.propagate<EvidenceEnvelope>();
  }
  envelope.value = decoded_value.value();

  if (const JsonValue* durability = member(document, "durability"); durability != nullptr && durability->is_string()) {
    auto parsed = durability_from_string(durability->string_or(std::string_view{}));
    if (!parsed) {
      return parsed.propagate<EvidenceEnvelope>();
    }
    envelope.durability = parsed.value();
  }
  if (const JsonValue* origin = member(document, "origin"); origin != nullptr && origin->is_string()) {
    envelope.origin.assign(origin->string_or(std::string_view{}));
  }
  if (const JsonValue* explanation = member(document, "explanation");
      explanation != nullptr && explanation->is_string()) {
    envelope.explanation.assign(explanation->string_or(std::string_view{}));
  }

  auto boundary = assert_envelope_boundary(envelope);
  if (!boundary) {
    return boundary.propagate<EvidenceEnvelope>();
  }
  return Outcome<EvidenceEnvelope>::ok(std::move(envelope));
}

std::string_view to_string(IngestDocument::Kind kind) noexcept {
  switch (kind) {
    case IngestDocument::Kind::authority:
      return std::string_view{"authority"};
    case IngestDocument::Kind::epoch:
      return std::string_view{"epoch"};
    case IngestDocument::Kind::evidence:
      return std::string_view{"evidence"};
  }
  return std::string_view{"unknown"};
}

JsonValue ingest_document_to_json(const IngestDocument& document) {
  if (document.kind == IngestDocument::Kind::authority) {
    JsonValue object = JsonValue::make_object();
    object.set("kind", JsonValue::make_string("authority"));
    object.set("id", JsonValue::make_string(document.authority.id.str()));
    object.set("authority_kind", JsonValue::make_string(std::string(to_string(document.authority.kind))));
    object.set("role", JsonValue::make_string(std::string(to_string(document.authority.role))));
    object.set("label", JsonValue::make_string(document.authority.label.str()));
    object.set("epoch", JsonValue::make_integer(static_cast<std::int64_t>(document.authority.epoch.value())));
    object.set("synthetic", JsonValue::make_boolean(document.authority.synthetic));
    return object;
  }
  if (document.kind == IngestDocument::Kind::epoch) {
    JsonValue object = JsonValue::make_object();
    object.set("kind", JsonValue::make_string("epoch"));
    object.set("authority", JsonValue::make_string(document.epoch_authority.str()));
    object.set("epoch", JsonValue::make_integer(static_cast<std::int64_t>(document.epoch.value())));
    object.set("published_at", JsonValue::make_string(document.published_at.to_rfc3339()));
    return object;
  }
  return envelope_to_json(document.envelope);
}

Outcome<IngestDocument> ingest_document_from_json(const JsonValue& document) {
  if (!document.is_object()) {
    return Outcome<IngestDocument>::fail(ReasonCode::malformed_encoding, "an ingest document must be an object");
  }
  const JsonValue* kind = document.find("kind");
  if (kind == nullptr || !kind->is_string()) {
    return Outcome<IngestDocument>::fail(ReasonCode::empty_input, "'kind' is required and must be a string");
  }
  const std::string_view kind_name = kind->string_or(std::string_view{});

  IngestDocument result;
  if (kind_name == "authority") {
    result.kind = IngestDocument::Kind::authority;
    auto id = required_string(document, "id");
    if (!id) {
      return id.propagate<IngestDocument>();
    }
    auto authority_id = AuthorityId::parse(id.value());
    if (!authority_id) {
      return authority_id.propagate<IngestDocument>();
    }
    result.authority.id = authority_id.value();

    auto authority_kind = required_string(document, "authority_kind");
    if (!authority_kind) {
      return authority_kind.propagate<IngestDocument>();
    }
    auto parsed_kind = authority_kind_from_string(authority_kind.value());
    if (!parsed_kind) {
      return parsed_kind.propagate<IngestDocument>();
    }
    result.authority.kind = parsed_kind.value();

    auto role = required_string(document, "role");
    if (!role) {
      return role.propagate<IngestDocument>();
    }
    auto parsed_role = authority_role_from_string(role.value());
    if (!parsed_role) {
      return parsed_role.propagate<IngestDocument>();
    }
    result.authority.role = parsed_role.value();

    if (const JsonValue* label = document.find("label"); label != nullptr && label->is_string()) {
      auto token = Token::parse(label->string_or(std::string_view{}));
      if (!token) {
        return token.propagate<IngestDocument>();
      }
      result.authority.label = token.value();
    }
    auto epoch = required_counter(document, "epoch");
    if (!epoch) {
      return epoch.propagate<IngestDocument>();
    }
    result.authority.epoch = Epoch::from_value(epoch.value());
    if (const JsonValue* synthetic = document.find("synthetic"); synthetic != nullptr) {
      result.authority.synthetic = synthetic->boolean_or(false);
    }
    if (result.authority.kind == AuthorityKind::synthetic) {
      result.authority.synthetic = true;
      if (result.authority.role == AuthorityRole::authority) {
        result.authority.role = AuthorityRole::synthetic;
      }
    }
    return Outcome<IngestDocument>::ok(std::move(result));
  }

  if (kind_name == "epoch") {
    result.kind = IngestDocument::Kind::epoch;
    auto authority = required_string(document, "authority");
    if (!authority) {
      return authority.propagate<IngestDocument>();
    }
    auto authority_id = AuthorityId::parse(authority.value());
    if (!authority_id) {
      return authority_id.propagate<IngestDocument>();
    }
    result.epoch_authority = authority_id.value();
    auto epoch = required_counter(document, "epoch");
    if (!epoch) {
      return epoch.propagate<IngestDocument>();
    }
    result.epoch = Epoch::from_value(epoch.value());
    auto published = required_string(document, "published_at");
    if (!published) {
      return published.propagate<IngestDocument>();
    }
    auto published_at = Timestamp::parse_rfc3339(published.value());
    if (!published_at) {
      return published_at.propagate<IngestDocument>();
    }
    result.published_at = published_at.value();
    return Outcome<IngestDocument>::ok(std::move(result));
  }

  if (kind_name == "evidence") {
    result.kind = IngestDocument::Kind::evidence;
    auto envelope = envelope_from_json(document);
    if (!envelope) {
      return envelope.propagate<IngestDocument>();
    }
    result.envelope = envelope.value();
    return Outcome<IngestDocument>::ok(std::move(result));
  }

  return Outcome<IngestDocument>::fail(ReasonCode::unsupported_query,
                                       "unknown ingest document kind '" + std::string(kind_name) + "'");
}

}  // namespace fo
