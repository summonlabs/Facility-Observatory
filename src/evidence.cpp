// Facility Observatory - evidence model.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/evidence.hpp"

#include <array>
#include <cstddef>

namespace fo {
namespace {

struct DomainName {
  Domain domain;
  std::string_view name;
};

constexpr std::array<DomainName, 8> kDomainNames{{
    {Domain::facility, "facility"},
    {Domain::site, "site"},
    {Domain::rack, "rack"},
    {Domain::asset, "asset"},
    {Domain::incident, "incident"},
    {Domain::capacity, "capacity"},
    {Domain::dependency, "dependency"},
    {Domain::unknown, "unknown"},
}};

}  // namespace

std::string_view to_string(Domain domain) noexcept {
  for (const DomainName& entry : kDomainNames) {
    if (entry.domain == domain) {
      return entry.name;
    }
  }
  return std::string_view{"unknown"};
}

Outcome<Domain> domain_from_string(std::string_view text) {
  for (const DomainName& entry : kDomainNames) {
    if (entry.name == text && entry.domain != Domain::unknown) {
      return Outcome<Domain>::ok(entry.domain);
    }
  }
  return Outcome<Domain>::fail(ReasonCode::unknown_domain, "unknown domain '" + std::string(text) + "'");
}

Outcome<EntityRef> EntityRef::parse(std::string_view text) {
  const std::size_t separator = text.find(':');
  if (separator == std::string_view::npos) {
    return Outcome<EntityRef>::fail(ReasonCode::invalid_argument,
                                    "entity reference must be '<domain>:<id>'");
  }
  auto domain = domain_from_string(text.substr(0, separator));
  if (!domain) {
    return domain.propagate<EntityRef>();
  }
  auto id = EntityId::parse(text.substr(separator + 1));
  if (!id) {
    return id.propagate<EntityRef>();
  }
  EntityRef reference;
  reference.domain = domain.value();
  reference.id = id.value();
  return Outcome<EntityRef>::ok(reference);
}

std::string EntityRef::to_string() const {
  std::string result(fo::to_string(domain));
  result.push_back(':');
  result.append(id.str());
  return result;
}

std::string_view to_string(ValueKind kind) noexcept {
  switch (kind) {
    case ValueKind::absent:
      return std::string_view{"absent"};
    case ValueKind::scalar:
      return std::string_view{"scalar"};
    case ValueKind::enumeration:
      return std::string_view{"enumeration"};
    case ValueKind::text:
      return std::string_view{"text"};
    case ValueKind::boolean:
      return std::string_view{"boolean"};
  }
  return std::string_view{"unknown"};
}

Value Value::absent() { return Value{}; }

Outcome<Value> Value::make_scalar(const FixedPoint& magnitude, Unit unit) {
  if (!is_known_unit(unit)) {
    return Outcome<Value>::fail(ReasonCode::unit_incompatible, "scalar value carries an unknown unit");
  }
  Value value;
  value.kind_ = ValueKind::scalar;
  value.scalar_ = magnitude;
  value.unit_ = unit;
  return Outcome<Value>::ok(value);
}

Outcome<Value> Value::make_enumeration(const Token& token) {
  if (token.empty()) {
    return Outcome<Value>::fail(ReasonCode::empty_input, "enumeration value must not be empty");
  }
  Value value;
  value.kind_ = ValueKind::enumeration;
  value.enumeration_ = token;
  return Outcome<Value>::ok(value);
}

Outcome<Value> Value::make_text(std::string_view text) {
  if (text.size() > kMaxTextBytes) {
    return Outcome<Value>::fail(ReasonCode::record_too_large,
                                "text value of " + std::to_string(text.size()) + " bytes exceeds bound of " +
                                    std::to_string(kMaxTextBytes) + " bytes");
  }
  if (!is_valid_utf8(text)) {
    return Outcome<Value>::fail(ReasonCode::malformed_encoding, "text value is not well-formed UTF-8");
  }
  Value value;
  value.kind_ = ValueKind::text;
  value.text_.assign(text);
  return Outcome<Value>::ok(value);
}

Outcome<Value> Value::make_boolean(bool flag) {
  Value value;
  value.kind_ = ValueKind::boolean;
  value.boolean_ = flag;
  return Outcome<Value>::ok(value);
}

std::string Value::to_string() const {
  switch (kind_) {
    case ValueKind::absent:
      return std::string("absent");
    case ValueKind::scalar: {
      std::string result = scalar_.to_string();
      result.push_back(' ');
      result.append(fo::to_string(unit_));
      return result;
    }
    case ValueKind::enumeration:
      return std::string("enum:") + enumeration_.str();
    case ValueKind::text:
      return std::string("text:") + text_;
    case ValueKind::boolean:
      return boolean_ ? std::string("true") : std::string("false");
  }
  return std::string("unknown");
}

bool operator==(const Value& lhs, const Value& rhs) noexcept {
  if (lhs.kind_ != rhs.kind_) {
    return false;
  }
  switch (lhs.kind_) {
    case ValueKind::absent:
      return true;
    case ValueKind::scalar:
      return lhs.scalar_ == rhs.scalar_ && lhs.unit_ == rhs.unit_;
    case ValueKind::enumeration:
      return lhs.enumeration_ == rhs.enumeration_;
    case ValueKind::text:
      return lhs.text_ == rhs.text_;
    case ValueKind::boolean:
      return lhs.boolean_ == rhs.boolean_;
  }
  return false;
}

std::string_view to_string(Durability durability) noexcept {
  switch (durability) {
    case Durability::live:
      return std::string_view{"live"};
    case Durability::recovered:
      return std::string_view{"recovered"};
    case Durability::synthetic:
      return std::string_view{"synthetic"};
    case Durability::imported:
      return std::string_view{"imported"};
  }
  return std::string_view{"unknown"};
}

Outcome<Durability> durability_from_string(std::string_view text) {
  if (text == "live") {
    return Outcome<Durability>::ok(Durability::live);
  }
  if (text == "recovered") {
    return Outcome<Durability>::ok(Durability::recovered);
  }
  if (text == "synthetic") {
    return Outcome<Durability>::ok(Durability::synthetic);
  }
  if (text == "imported") {
    return Outcome<Durability>::ok(Durability::imported);
  }
  return Outcome<Durability>::fail(ReasonCode::invalid_argument,
                                   "unknown durability '" + std::string(text) + "'");
}

bool same_publication(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
  return lhs.key == rhs.key && lhs.epoch == rhs.epoch && lhs.generation == rhs.generation &&
         lhs.revision == rhs.revision && lhs.observed_at == rhs.observed_at &&
         lhs.published_at == rhs.published_at && lhs.value == rhs.value;
}

bool canonical_less(const EvidenceRecord& lhs, const EvidenceRecord& rhs) noexcept {
  if (lhs.key.entity != rhs.key.entity) {
    return lhs.key.entity < rhs.key.entity;
  }
  if (lhs.key.aspect != rhs.key.aspect) {
    return lhs.key.aspect < rhs.key.aspect;
  }
  if (lhs.key.authority != rhs.key.authority) {
    return lhs.key.authority < rhs.key.authority;
  }
  if (lhs.key.source != rhs.key.source) {
    return lhs.key.source < rhs.key.source;
  }
  if (lhs.epoch != rhs.epoch) {
    return lhs.epoch < rhs.epoch;
  }
  if (lhs.generation != rhs.generation) {
    return lhs.generation < rhs.generation;
  }
  if (lhs.revision != rhs.revision) {
    return lhs.revision < rhs.revision;
  }
  if (lhs.observed_at != rhs.observed_at) {
    return lhs.observed_at < rhs.observed_at;
  }
  if (lhs.published_at != rhs.published_at) {
    return lhs.published_at < rhs.published_at;
  }
  // Final tie-break: two records may agree on every position yet disagree on
  // the claim itself. Ordering by the rendered value keeps the total order
  // total, so every derived output stays deterministic.
  const std::string lhs_value = lhs.value.to_string();
  const std::string rhs_value = rhs.value.to_string();
  if (lhs_value != rhs_value) {
    return lhs_value < rhs_value;
  }
  return lhs.explanation < rhs.explanation;
}

bool value_equivalent(const Value& lhs, const Value& rhs) {
  if (lhs.kind() != rhs.kind()) {
    return false;
  }
  if (lhs.kind() != ValueKind::scalar) {
    return lhs == rhs;
  }
  if (!units_compatible(lhs.unit(), rhs.unit())) {
    return false;
  }
  auto ordering = lhs.scalar_magnitude().compare(rhs.scalar_magnitude());
  if (!ordering) {
    return false;
  }
  return ordering.value() == std::strong_ordering::equal;
}

std::string EvidenceRef::to_string() const {
  std::string result;
  result.reserve(192);
  result.append(key.authority.str());
  result.push_back('/');
  result.append(key.source.str());
  result.push_back('|');
  result.append(key.entity.to_string());
  result.push_back('|');
  result.append(key.aspect.str());
  result.append("|epoch=");
  result.append(std::to_string(epoch.value()));
  result.append("|generation=");
  result.append(std::to_string(generation.value()));
  result.append("|revision=");
  result.append(std::to_string(revision.value()));
  result.append("|observed=");
  result.append(observed_at.to_rfc3339());
  result.append("|published=");
  result.append(published_at.to_rfc3339());
  result.append("|durability=");
  result.append(fo::to_string(durability));
  result.append("|sequence=");
  result.append(std::to_string(sequence.value()));
  return result;
}

EvidenceRef to_reference(const EvidenceRecord& record) {
  EvidenceRef reference;
  reference.key = record.key;
  reference.epoch = record.epoch;
  reference.generation = record.generation;
  reference.revision = record.revision;
  reference.observed_at = record.observed_at;
  reference.published_at = record.published_at;
  reference.durability = record.provenance.durability;
  reference.sequence = record.provenance.sequence;
  return reference;
}

std::string_view to_string(ObservationState state) noexcept {
  switch (state) {
    case ObservationState::known:
      return std::string_view{"known"};
    case ObservationState::stale:
      return std::string_view{"stale"};
    case ObservationState::unknown:
      return std::string_view{"unknown"};
    case ObservationState::conflicting:
      return std::string_view{"conflicting"};
    case ObservationState::unsupported:
      return std::string_view{"unsupported"};
    case ObservationState::indeterminate:
      return std::string_view{"indeterminate"};
  }
  return std::string_view{"unknown"};
}

Outcome<ObservationState> observation_state_from_string(std::string_view text) {
  const std::array<std::pair<std::string_view, ObservationState>, 6> table{{
      {"known", ObservationState::known},
      {"stale", ObservationState::stale},
      {"unknown", ObservationState::unknown},
      {"conflicting", ObservationState::conflicting},
      {"unsupported", ObservationState::unsupported},
      {"indeterminate", ObservationState::indeterminate},
  }};
  for (const auto& entry : table) {
    if (entry.first == text) {
      return Outcome<ObservationState>::ok(entry.second);
    }
  }
  return Outcome<ObservationState>::fail(ReasonCode::invalid_argument,
                                         "unknown observation state '" + std::string(text) + "'");
}

}  // namespace fo
