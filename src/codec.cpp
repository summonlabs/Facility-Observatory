// Facility Observatory - explicit binary codec.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/codec.hpp"

#include <string>
#include <utility>

namespace fo {
namespace {

// Mirrors the default bound in StoreLimits; the codec rejects an oversized
// explanation before it can reach the store.
constexpr std::size_t kMaxExplanationBytes = 1024;

Outcome<Domain> domain_from_u8(std::uint8_t raw) {
  switch (raw) {
    case 0:
      return Outcome<Domain>::ok(Domain::facility);
    case 1:
      return Outcome<Domain>::ok(Domain::site);
    case 2:
      return Outcome<Domain>::ok(Domain::rack);
    case 3:
      return Outcome<Domain>::ok(Domain::asset);
    case 4:
      return Outcome<Domain>::ok(Domain::incident);
    case 5:
      return Outcome<Domain>::ok(Domain::capacity);
    case 6:
      return Outcome<Domain>::ok(Domain::dependency);
    default:
      return Outcome<Domain>::fail(ReasonCode::malformed_encoding,
                                   "domain code " + std::to_string(raw) + " is not defined");
  }
}

Outcome<Unit> unit_from_u8(std::uint8_t raw) {
  const auto unit = static_cast<Unit>(raw);
  if (!is_known_unit(unit)) {
    return Outcome<Unit>::fail(ReasonCode::malformed_encoding,
                               "unit code " + std::to_string(raw) + " is not defined");
  }
  return Outcome<Unit>::ok(unit);
}

Outcome<AuthorityKind> kind_from_u8(std::uint8_t raw) {
  switch (raw) {
    case 0:
      return Outcome<AuthorityKind>::ok(AuthorityKind::dccp);
    case 1:
      return Outcome<AuthorityKind>::ok(AuthorityKind::asi);
    case 2:
      return Outcome<AuthorityKind>::ok(AuthorityKind::dfi);
    case 3:
      return Outcome<AuthorityKind>::ok(AuthorityKind::bms);
    case 4:
      return Outcome<AuthorityKind>::ok(AuthorityKind::dcim);
    case 5:
      return Outcome<AuthorityKind>::ok(AuthorityKind::plant);
    case 6:
      return Outcome<AuthorityKind>::ok(AuthorityKind::economic);
    case 7:
      return Outcome<AuthorityKind>::ok(AuthorityKind::synthetic);
    case 255:
      return Outcome<AuthorityKind>::ok(AuthorityKind::unknown);
    default:
      return Outcome<AuthorityKind>::fail(ReasonCode::malformed_encoding,
                                          "authority kind code " + std::to_string(raw) + " is not defined");
  }
}

Outcome<AuthorityRole> role_from_u8(std::uint8_t raw) {
  switch (raw) {
    case 0:
      return Outcome<AuthorityRole>::ok(AuthorityRole::authority);
    case 1:
      return Outcome<AuthorityRole>::ok(AuthorityRole::advisory);
    case 2:
      return Outcome<AuthorityRole>::ok(AuthorityRole::synthetic);
    case 255:
      return Outcome<AuthorityRole>::ok(AuthorityRole::unknown);
    default:
      return Outcome<AuthorityRole>::fail(ReasonCode::malformed_encoding,
                                          "authority role code " + std::to_string(raw) + " is not defined");
  }
}

Outcome<Durability> durability_from_u8(std::uint8_t raw) {
  switch (raw) {
    case 0:
      return Outcome<Durability>::ok(Durability::live);
    case 1:
      return Outcome<Durability>::ok(Durability::recovered);
    case 2:
      return Outcome<Durability>::ok(Durability::synthetic);
    case 3:
      return Outcome<Durability>::ok(Durability::imported);
    default:
      return Outcome<Durability>::fail(ReasonCode::malformed_encoding,
                                       "durability code " + std::to_string(raw) + " is not defined");
  }
}

Outcome<AspectClass> aspect_class_from_u8(std::uint8_t raw) {
  switch (raw) {
    case 0:
      return Outcome<AspectClass>::ok(AspectClass::static_identity);
    case 1:
      return Outcome<AspectClass>::ok(AspectClass::dynamic_measurement);
    default:
      return Outcome<AspectClass>::fail(ReasonCode::malformed_encoding,
                                        "aspect class code " + std::to_string(raw) + " is not defined");
  }
}

void encode_value(Encoder& encoder, const Value& value) {
  encoder.u8(static_cast<std::uint8_t>(value.kind()));
  switch (value.kind()) {
    case ValueKind::absent:
      break;
    case ValueKind::scalar:
      encoder.i64(value.scalar_magnitude().mantissa());
      encoder.u8(static_cast<std::uint8_t>(value.scalar_magnitude().scale()));
      encoder.u8(static_cast<std::uint8_t>(value.unit()));
      break;
    case ValueKind::enumeration:
      encoder.text(value.enumeration_token().str());
      break;
    case ValueKind::text:
      encoder.text(value.text_body());
      break;
    case ValueKind::boolean:
      encoder.boolean(value.boolean_flag());
      break;
  }
}

Outcome<Value> decode_value(Decoder& decoder) {
  auto kind_code = decoder.u8();
  if (!kind_code) {
    return kind_code.propagate<Value>();
  }
  switch (static_cast<ValueKind>(kind_code.value())) {
    case ValueKind::absent:
      return Outcome<Value>::ok(Value::absent());
    case ValueKind::scalar: {
      auto mantissa = decoder.i64();
      if (!mantissa) {
        return mantissa.propagate<Value>();
      }
      auto scale = decoder.u8();
      if (!scale) {
        return scale.propagate<Value>();
      }
      auto unit = decoder.u8();
      if (!unit) {
        return unit.propagate<Value>();
      }
      auto resolved_unit = unit_from_u8(unit.value());
      if (!resolved_unit) {
        return resolved_unit.propagate<Value>();
      }
      auto magnitude = FixedPoint::from_scaled(mantissa.value(), static_cast<int>(scale.value()));
      if (!magnitude) {
        return magnitude.propagate<Value>();
      }
      return Value::make_scalar(magnitude.value(), resolved_unit.value());
    }
    case ValueKind::enumeration: {
      auto token = decoder.text();
      if (!token) {
        return token.propagate<Value>();
      }
      auto parsed = Token::decode(token.value());
      if (!parsed) {
        return parsed.propagate<Value>();
      }
      return Value::make_enumeration(parsed.value());
    }
    case ValueKind::text: {
      auto body = decoder.text();
      if (!body) {
        return body.propagate<Value>();
      }
      return Value::make_text(body.value());
    }
    case ValueKind::boolean: {
      auto flag = decoder.boolean();
      if (!flag) {
        return flag.propagate<Value>();
      }
      return Value::make_boolean(flag.value());
    }
  }
  return Outcome<Value>::fail(ReasonCode::malformed_encoding, "value kind is not defined");
}

}  // namespace

void Encoder::u8(std::uint8_t value) { buffer_.push_back(value); }

void Encoder::u16(std::uint16_t value) {
  buffer_.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  buffer_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFFU));
}

void Encoder::u32(std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void Encoder::u64(std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    buffer_.push_back(static_cast<std::uint8_t>((value >> shift) & 0xFFU));
  }
}

void Encoder::i64(std::int64_t value) { u64(static_cast<std::uint64_t>(value)); }

void Encoder::boolean(bool value) { u8(value ? 1U : 0U); }

void Encoder::sized_bytes(std::string_view value) {
  u32(static_cast<std::uint32_t>(value.size()));
  buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void Encoder::text(const std::string& value) { sized_bytes(std::string_view(value)); }

void Encoder::timestamp(const Timestamp& value) { i64(value.unix_nanos()); }

Outcome<std::span<const std::uint8_t>> Decoder::take(std::size_t count) {
  if (count > remaining()) {
    return Outcome<std::span<const std::uint8_t>>::fail(
        ReasonCode::malformed_encoding,
        "decoder needs " + std::to_string(count) + " bytes but only " + std::to_string(remaining()) + " remain");
  }
  const std::span<const std::uint8_t> slice = input_.subspan(cursor_, count);
  cursor_ += count;
  return Outcome<std::span<const std::uint8_t>>::ok(slice);
}

Outcome<std::uint8_t> Decoder::u8() {
  auto slice = take(1);
  if (!slice) {
    return slice.propagate<std::uint8_t>();
  }
  return Outcome<std::uint8_t>::ok(slice.value()[0]);
}

Outcome<std::uint16_t> Decoder::u16() {
  auto slice = take(2);
  if (!slice) {
    return slice.propagate<std::uint16_t>();
  }
  const std::uint16_t value = static_cast<std::uint16_t>(slice.value()[0]) |
                              static_cast<std::uint16_t>(static_cast<std::uint16_t>(slice.value()[1]) << 8);
  return Outcome<std::uint16_t>::ok(value);
}

Outcome<std::uint32_t> Decoder::u32() {
  auto slice = take(4);
  if (!slice) {
    return slice.propagate<std::uint32_t>();
  }
  std::uint32_t value = 0;
  for (int index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(slice.value()[static_cast<std::size_t>(index)]) << (8 * index);
  }
  return Outcome<std::uint32_t>::ok(value);
}

Outcome<std::uint64_t> Decoder::u64() {
  auto slice = take(8);
  if (!slice) {
    return slice.propagate<std::uint64_t>();
  }
  std::uint64_t value = 0;
  for (int index = 0; index < 8; ++index) {
    value |= static_cast<std::uint64_t>(slice.value()[static_cast<std::size_t>(index)]) << (8 * index);
  }
  return Outcome<std::uint64_t>::ok(value);
}

Outcome<std::int64_t> Decoder::i64() {
  auto raw = u64();
  if (!raw) {
    return raw.propagate<std::int64_t>();
  }
  return Outcome<std::int64_t>::ok(static_cast<std::int64_t>(raw.value()));
}

Outcome<bool> Decoder::boolean() {
  auto raw = u8();
  if (!raw) {
    return raw.propagate<bool>();
  }
  if (raw.value() > 1U) {
    return Outcome<bool>::fail(ReasonCode::malformed_encoding,
                               "boolean encoding must be 0 or 1, found " + std::to_string(raw.value()));
  }
  return Outcome<bool>::ok(raw.value() == 1U);
}

Outcome<std::string_view> Decoder::sized_bytes() {
  auto length = u32();
  if (!length) {
    return length.propagate<std::string_view>();
  }
  auto slice = take(static_cast<std::size_t>(length.value()));
  if (!slice) {
    return slice.propagate<std::string_view>();
  }
  return Outcome<std::string_view>::ok(
      std::string_view(reinterpret_cast<const char*>(slice.value().data()), slice.value().size()));
}

Outcome<std::string> Decoder::text() {
  auto view = sized_bytes();
  if (!view) {
    return view.propagate<std::string>();
  }
  return Outcome<std::string>::ok(std::string(view.value()));
}

Outcome<Timestamp> Decoder::timestamp() {
  auto raw = i64();
  if (!raw) {
    return raw.propagate<Timestamp>();
  }
  if (raw.value() == Timestamp::kUnsetValue) {
    return Outcome<Timestamp>::ok(Timestamp{});
  }
  return Timestamp::from_unix_nanos(raw.value());
}

Status Decoder::require_end() const {
  if (!at_end()) {
    return Status::fail(ReasonCode::malformed_encoding,
                        std::to_string(remaining()) + " trailing bytes remain after decoding");
  }
  return success();
}

std::vector<std::uint8_t> encode_evidence(const EvidenceRecord& record) {
  Encoder encoder;
  encoder.text(record.key.authority.str());
  encoder.text(record.key.source.str());
  encoder.u8(static_cast<std::uint8_t>(record.key.entity.domain));
  encoder.text(record.key.entity.id.str());
  encoder.text(record.key.aspect.str());
  encoder.u64(record.epoch.value());
  encoder.u64(record.generation.value());
  encoder.u64(record.revision.value());
  encoder.timestamp(record.observed_at);
  encoder.timestamp(record.published_at);
  encode_value(encoder, record.value);
  encoder.timestamp(record.provenance.received_at);
  encoder.u8(static_cast<std::uint8_t>(record.provenance.durability));
  encoder.u64(record.provenance.sequence.value());
  encoder.u64(record.provenance.epoch_at_receipt.value());
  encoder.text(record.provenance.origin);
  encoder.text(record.explanation);
  return encoder.buffer();
}

Outcome<EvidenceRecord> decode_evidence(std::span<const std::uint8_t> bytes) {
  Decoder decoder(bytes);
  EvidenceRecord record;

  auto authority = decoder.text();
  if (!authority) {
    return authority.propagate<EvidenceRecord>();
  }
  auto authority_id = AuthorityId::decode(authority.value());
  if (!authority_id) {
    return authority_id.propagate<EvidenceRecord>();
  }
  record.key.authority = authority_id.value();

  auto source = decoder.text();
  if (!source) {
    return source.propagate<EvidenceRecord>();
  }
  auto source_id = SourceId::decode(source.value());
  if (!source_id) {
    return source_id.propagate<EvidenceRecord>();
  }
  record.key.source = source_id.value();

  auto domain = decoder.u8();
  if (!domain) {
    return domain.propagate<EvidenceRecord>();
  }
  auto resolved_domain = domain_from_u8(domain.value());
  if (!resolved_domain) {
    return resolved_domain.propagate<EvidenceRecord>();
  }
  record.key.entity.domain = resolved_domain.value();

  auto entity = decoder.text();
  if (!entity) {
    return entity.propagate<EvidenceRecord>();
  }
  auto entity_id = EntityId::decode(entity.value());
  if (!entity_id) {
    return entity_id.propagate<EvidenceRecord>();
  }
  record.key.entity.id = entity_id.value();

  auto aspect = decoder.text();
  if (!aspect) {
    return aspect.propagate<EvidenceRecord>();
  }
  auto aspect_id = AspectId::decode(aspect.value());
  if (!aspect_id) {
    return aspect_id.propagate<EvidenceRecord>();
  }
  record.key.aspect = aspect_id.value();

  auto epoch = decoder.u64();
  if (!epoch) {
    return epoch.propagate<EvidenceRecord>();
  }
  record.epoch = Epoch::from_value(epoch.value());

  auto generation = decoder.u64();
  if (!generation) {
    return generation.propagate<EvidenceRecord>();
  }
  record.generation = Generation::from_value(generation.value());

  auto revision = decoder.u64();
  if (!revision) {
    return revision.propagate<EvidenceRecord>();
  }
  record.revision = Revision::from_value(revision.value());

  auto observed = decoder.timestamp();
  if (!observed) {
    return observed.propagate<EvidenceRecord>();
  }
  record.observed_at = observed.value();

  auto published = decoder.timestamp();
  if (!published) {
    return published.propagate<EvidenceRecord>();
  }
  record.published_at = published.value();

  auto value = decode_value(decoder);
  if (!value) {
    return value.propagate<EvidenceRecord>();
  }
  record.value = value.value();

  auto received = decoder.timestamp();
  if (!received) {
    return received.propagate<EvidenceRecord>();
  }
  record.provenance.received_at = received.value();

  auto durability = decoder.u8();
  if (!durability) {
    return durability.propagate<EvidenceRecord>();
  }
  auto resolved_durability = durability_from_u8(durability.value());
  if (!resolved_durability) {
    return resolved_durability.propagate<EvidenceRecord>();
  }
  record.provenance.durability = resolved_durability.value();

  auto sequence = decoder.u64();
  if (!sequence) {
    return sequence.propagate<EvidenceRecord>();
  }
  record.provenance.sequence = JournalSequence::from_value(sequence.value());

  auto receipt_epoch = decoder.u64();
  if (!receipt_epoch) {
    return receipt_epoch.propagate<EvidenceRecord>();
  }
  record.provenance.epoch_at_receipt = Epoch::from_value(receipt_epoch.value());

  auto origin = decoder.text();
  if (!origin) {
    return origin.propagate<EvidenceRecord>();
  }
  record.provenance.origin = origin.value();

  auto explanation = decoder.text();
  if (!explanation) {
    return explanation.propagate<EvidenceRecord>();
  }
  record.explanation = explanation.value();

  if (explanation.value().size() > kMaxExplanationBytes) {
    return Outcome<EvidenceRecord>::fail(ReasonCode::record_too_large, "explanation exceeds its bound");
  }

  auto finished = decoder.require_end();
  if (!finished) {
    return finished.propagate<EvidenceRecord>();
  }
  return Outcome<EvidenceRecord>::ok(std::move(record));
}

std::vector<std::uint8_t> encode_authority(const AuthorityDescriptor& descriptor) {
  Encoder encoder;
  encoder.text(descriptor.id.str());
  encoder.u8(static_cast<std::uint8_t>(descriptor.kind));
  encoder.u8(static_cast<std::uint8_t>(descriptor.role));
  encoder.text(descriptor.label.str());
  encoder.u64(descriptor.epoch.value());
  encoder.timestamp(descriptor.epoch_published_at);
  encoder.boolean(descriptor.synthetic);
  return encoder.buffer();
}

Outcome<AuthorityDescriptor> decode_authority(std::span<const std::uint8_t> bytes) {
  Decoder decoder(bytes);
  AuthorityDescriptor descriptor;

  auto id = decoder.text();
  if (!id) {
    return id.propagate<AuthorityDescriptor>();
  }
  auto authority_id = AuthorityId::decode(id.value());
  if (!authority_id) {
    return authority_id.propagate<AuthorityDescriptor>();
  }
  descriptor.id = authority_id.value();

  auto kind = decoder.u8();
  if (!kind) {
    return kind.propagate<AuthorityDescriptor>();
  }
  auto resolved_kind = kind_from_u8(kind.value());
  if (!resolved_kind) {
    return resolved_kind.propagate<AuthorityDescriptor>();
  }
  descriptor.kind = resolved_kind.value();

  auto role = decoder.u8();
  if (!role) {
    return role.propagate<AuthorityDescriptor>();
  }
  auto resolved_role = role_from_u8(role.value());
  if (!resolved_role) {
    return resolved_role.propagate<AuthorityDescriptor>();
  }
  descriptor.role = resolved_role.value();

  auto label = decoder.text();
  if (!label) {
    return label.propagate<AuthorityDescriptor>();
  }
  if (!label.value().empty()) {
    auto token = Token::decode(label.value());
    if (!token) {
      return token.propagate<AuthorityDescriptor>();
    }
    descriptor.label = token.value();
  }

  auto epoch = decoder.u64();
  if (!epoch) {
    return epoch.propagate<AuthorityDescriptor>();
  }
  descriptor.epoch = Epoch::from_value(epoch.value());

  auto published = decoder.timestamp();
  if (!published) {
    return published.propagate<AuthorityDescriptor>();
  }
  descriptor.epoch_published_at = published.value();

  auto synthetic = decoder.boolean();
  if (!synthetic) {
    return synthetic.propagate<AuthorityDescriptor>();
  }
  descriptor.synthetic = synthetic.value();

  auto finished = decoder.require_end();
  if (!finished) {
    return finished.propagate<AuthorityDescriptor>();
  }
  return Outcome<AuthorityDescriptor>::ok(std::move(descriptor));
}

std::vector<std::uint8_t> encode_epoch_advance(const AuthorityId& authority, Epoch epoch,
                                               Timestamp published_at) {
  Encoder encoder;
  encoder.text(authority.str());
  encoder.u64(epoch.value());
  encoder.timestamp(published_at);
  return encoder.buffer();
}

Outcome<Nothing> decode_epoch_advance(std::span<const std::uint8_t> bytes, AuthorityId& authority, Epoch& epoch,
                                      Timestamp& published_at) {
  Decoder decoder(bytes);
  auto id = decoder.text();
  if (!id) {
    return id.propagate<Nothing>();
  }
  auto authority_id = AuthorityId::decode(id.value());
  if (!authority_id) {
    return authority_id.propagate<Nothing>();
  }
  authority = authority_id.value();

  auto raw_epoch = decoder.u64();
  if (!raw_epoch) {
    return raw_epoch.propagate<Nothing>();
  }
  epoch = Epoch::from_value(raw_epoch.value());

  auto published = decoder.timestamp();
  if (!published) {
    return published.propagate<Nothing>();
  }
  published_at = published.value();
  return decoder.require_end();
}

std::vector<std::uint8_t> encode_policy(const AspectPolicy& policy) {
  Encoder encoder;
  encoder.text(policy.aspect.str());
  encoder.i64(policy.freshness.fresh_for.count());
  encoder.i64(policy.freshness.stale_after.count());
  encoder.i64(policy.freshness.future_skew.count());
  encoder.u8(static_cast<std::uint8_t>(policy.aspect_class));
  encoder.boolean(policy.canonical_unit.has_value());
  if (policy.canonical_unit.has_value()) {
    encoder.u8(static_cast<std::uint8_t>(policy.canonical_unit.value()));
  }
  encoder.u32(static_cast<std::uint32_t>(policy.precedence.size()));
  for (const AuthorityId& id : policy.precedence) {
    encoder.text(id.str());
  }
  return encoder.buffer();
}

Outcome<AspectPolicy> decode_policy(std::span<const std::uint8_t> bytes) {
  Decoder decoder(bytes);
  AspectPolicy policy;

  auto aspect = decoder.text();
  if (!aspect) {
    return aspect.propagate<AspectPolicy>();
  }
  if (!aspect.value().empty()) {
    auto aspect_id = AspectId::decode(aspect.value());
    if (!aspect_id) {
      return aspect_id.propagate<AspectPolicy>();
    }
    policy.aspect = aspect_id.value();
  }

  auto fresh_for = decoder.i64();
  if (!fresh_for) {
    return fresh_for.propagate<AspectPolicy>();
  }
  policy.freshness.fresh_for = Duration(fresh_for.value());

  auto stale_after = decoder.i64();
  if (!stale_after) {
    return stale_after.propagate<AspectPolicy>();
  }
  policy.freshness.stale_after = Duration(stale_after.value());

  auto future_skew = decoder.i64();
  if (!future_skew) {
    return future_skew.propagate<AspectPolicy>();
  }
  policy.freshness.future_skew = Duration(future_skew.value());

  auto aspect_class = decoder.u8();
  if (!aspect_class) {
    return aspect_class.propagate<AspectPolicy>();
  }
  auto resolved_class = aspect_class_from_u8(aspect_class.value());
  if (!resolved_class) {
    return resolved_class.propagate<AspectPolicy>();
  }
  policy.aspect_class = resolved_class.value();

  auto has_unit = decoder.boolean();
  if (!has_unit) {
    return has_unit.propagate<AspectPolicy>();
  }
  if (has_unit.value()) {
    auto unit = decoder.u8();
    if (!unit) {
      return unit.propagate<AspectPolicy>();
    }
    auto resolved_unit = unit_from_u8(unit.value());
    if (!resolved_unit) {
      return resolved_unit.propagate<AspectPolicy>();
    }
    policy.canonical_unit = resolved_unit.value();
  }

  auto count = decoder.u32();
  if (!count) {
    return count.propagate<AspectPolicy>();
  }
  if (count.value() > 4096U) {
    return Outcome<AspectPolicy>::fail(ReasonCode::record_too_large,
                                       "precedence list of " + std::to_string(count.value()) + " entries is absurd");
  }
  for (std::uint32_t index = 0; index < count.value(); ++index) {
    auto id = decoder.text();
    if (!id) {
      return id.propagate<AspectPolicy>();
    }
    auto authority_id = AuthorityId::decode(id.value());
    if (!authority_id) {
      return authority_id.propagate<AspectPolicy>();
    }
    policy.precedence.push_back(authority_id.value());
  }

  auto finished = decoder.require_end();
  if (!finished) {
    return finished.propagate<AspectPolicy>();
  }
  return Outcome<AspectPolicy>::ok(std::move(policy));
}

std::vector<std::uint8_t> encode_snapshot_marker(JournalSequence sequence, std::string_view digest,
                                                 Timestamp created_at) {
  Encoder encoder;
  encoder.u64(sequence.value());
  encoder.sized_bytes(digest);
  encoder.timestamp(created_at);
  return encoder.buffer();
}

Outcome<Nothing> decode_snapshot_marker(std::span<const std::uint8_t> bytes, JournalSequence& sequence,
                                        std::string& digest, Timestamp& created_at) {
  Decoder decoder(bytes);
  auto raw = decoder.u64();
  if (!raw) {
    return raw.propagate<Nothing>();
  }
  sequence = JournalSequence::from_value(raw.value());

  auto body = decoder.sized_bytes();
  if (!body) {
    return body.propagate<Nothing>();
  }
  digest.assign(body.value());

  auto stamp = decoder.timestamp();
  if (!stamp) {
    return stamp.propagate<Nothing>();
  }
  created_at = stamp.value();
  return decoder.require_end();
}

std::vector<std::uint8_t> encode_session_marker(std::string_view origin, Timestamp at) {
  Encoder encoder;
  encoder.sized_bytes(origin);
  encoder.timestamp(at);
  return encoder.buffer();
}

Outcome<Nothing> decode_session_marker(std::span<const std::uint8_t> bytes, std::string& origin, Timestamp& at) {
  Decoder decoder(bytes);
  auto body = decoder.sized_bytes();
  if (!body) {
    return body.propagate<Nothing>();
  }
  origin.assign(body.value());

  auto stamp = decoder.timestamp();
  if (!stamp) {
    return stamp.propagate<Nothing>();
  }
  at = stamp.value();
  return decoder.require_end();
}

}  // namespace fo
