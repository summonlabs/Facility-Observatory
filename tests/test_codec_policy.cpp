// Facility Observatory - codec, freshness, policy and contract tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include "facility_observatory/codec.hpp"
#include "facility_observatory/contracts.hpp"
#include "facility_observatory/policy.hpp"
#include "facility_observatory/store.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

FO_TEST(codec_evidence_round_trip_all_value_kinds) {
  const EvidenceRecord scalars =
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 3, 7, 2, "-1234.567", "kilowatts", 120);
  auto decoded = decode_evidence(encode_evidence(scalars));
  FO_REQUIRE(decoded.has_value());
  FO_REQUIRE(decoded.value() == scalars);

  EvidenceRecord text = scalars;
  text.value = Value::make_text("caf\xC3\xA9").value();
  auto decoded_text = decode_evidence(encode_evidence(text));
  FO_REQUIRE(decoded_text.has_value());
  FO_REQUIRE(decoded_text.value().value == text.value);

  EvidenceRecord flag = scalars;
  flag.value = Value::make_boolean(true).value();
  FO_REQUIRE(decode_evidence(encode_evidence(flag)).value().value == flag.value);

  EvidenceRecord token = scalars;
  token.value = Value::make_enumeration(Token::parse("degraded").value()).value();
  FO_REQUIRE(decode_evidence(encode_evidence(token)).value().value == token.value);

  EvidenceRecord absent = scalars;
  absent.value = Value::absent();
  FO_REQUIRE(decode_evidence(encode_evidence(absent)).value().value == absent.value);
  FO_REQUIRE(decode_evidence(encode_evidence(absent)).value().value.is_absent());
}

FO_TEST(codec_rejects_truncation_and_trailing_bytes) {
  const EvidenceRecord record =
      make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 10);
  const std::vector<std::uint8_t> encoded = encode_evidence(record);

  for (std::size_t cut = 0; cut < encoded.size(); ++cut) {
    std::vector<std::uint8_t> truncated(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(cut));
    auto decoded = decode_evidence(truncated);
    FO_REQUIRE_MESSAGE(!decoded.has_value(), "accepted a payload truncated to " + std::to_string(cut) + " bytes");
  }

  std::vector<std::uint8_t> extended = encoded;
  extended.push_back(0U);
  auto trailing = decode_evidence(extended);
  FO_REQUIRE(!trailing.has_value());
  FO_REQUIRE(trailing.code() == ReasonCode::malformed_encoding);
}

FO_TEST(codec_rejects_unknown_domain_codes) {
  EvidenceRecord record = make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1", "watts", 10);
  record.key.entity.domain = static_cast<Domain>(99);
  auto refused = decode_evidence(encode_evidence(record));
  FO_REQUIRE(!refused.has_value());
  FO_REQUIRE(refused.code() == ReasonCode::malformed_encoding);
}

FO_TEST(codec_rejects_malformed_utf8_identifiers) {
  const EvidenceRecord record =
      make_evidence("dccp-a", "telemetry", "rack:sensor-seventeen", "power.draw", 1, 1, 1, "1", "watts", 10);
  std::vector<std::uint8_t> payload = encode_evidence(record);

  const std::string needle = "sensor-seventeen";
  std::size_t position = payload.size();
  for (std::size_t start = 0; start + needle.size() <= payload.size(); ++start) {
    bool matched = true;
    for (std::size_t offset = 0; offset < needle.size(); ++offset) {
      if (payload[start + offset] != static_cast<std::uint8_t>(needle[offset])) {
        matched = false;
        break;
      }
    }
    if (matched) {
      position = start;
      break;
    }
  }
  FO_REQUIRE(position != payload.size());
  payload[position + 4] = 0xC0U;
  payload[position + 5] = 0x20U;
  auto decoded = decode_evidence(payload);
  FO_REQUIRE(!decoded.has_value());
  FO_REQUIRE(decoded.code() == ReasonCode::malformed_encoding);
}

FO_TEST(codec_auxiliary_records) {
  const AuthorityDescriptor descriptor = make_authority("dccp-a", AuthorityKind::dccp, 5);
  auto decoded_authority = decode_authority(encode_authority(descriptor));
  FO_REQUIRE(decoded_authority.has_value());
  FO_REQUIRE(decoded_authority.value() == descriptor);

  const std::vector<std::uint8_t> epoch_payload =
      encode_epoch_advance(descriptor.id, Epoch::from_value(9), at_seconds(4));
  AuthorityId id;
  Epoch epoch;
  Timestamp stamp;
  FO_REQUIRE(decode_epoch_advance(epoch_payload, id, epoch, stamp).has_value());
  FO_REQUIRE(id == descriptor.id);
  FO_REQUIRE_EQ(epoch.value(), 9);
  FO_REQUIRE_EQ(stamp.to_rfc3339(), std::string("1970-01-01T00:00:04Z"));

  AspectPolicy policy = make_policy("power.draw", 5, 30, AspectClass::static_identity);
  policy.canonical_unit = Unit::watts;
  policy.precedence.push_back(AuthorityId::parse("dccp-a").value());
  policy.precedence.push_back(AuthorityId::parse("bms-a").value());
  auto decoded_policy = decode_policy(encode_policy(policy));
  FO_REQUIRE(decoded_policy.has_value());
  FO_REQUIRE(decoded_policy.value() == policy);

  JournalSequence sequence;
  std::string digest;
  Timestamp created;
  FO_REQUIRE(decode_snapshot_marker(encode_snapshot_marker(JournalSequence::from_value(42), "abcdef", at_seconds(7)),
                                    sequence, digest, created)
                 .has_value());
  FO_REQUIRE_EQ(sequence.value(), 42);
  FO_REQUIRE_EQ(digest, std::string("abcdef"));

  std::string origin;
  FO_REQUIRE(decode_session_marker(encode_session_marker("cli:open", at_seconds(2)), origin, created).has_value());
  FO_REQUIRE_EQ(origin, std::string("cli:open"));
}

FO_TEST(freshness_verdict_boundaries) {
  FreshnessWindow window;
  window.fresh_for = std::chrono::seconds(10);
  window.stale_after = std::chrono::seconds(100);
  window.future_skew = std::chrono::seconds(5);
  FO_REQUIRE(validate_freshness_window(window).has_value());

  const Timestamp observed = at_seconds(1000);
  const auto verdict_at = [&](std::int64_t when) {
    return assess_freshness(window, observed, observed, at_seconds(when)).verdict;
  };
  FO_REQUIRE(verdict_at(1000) == FreshnessVerdict::fresh);
  FO_REQUIRE(verdict_at(1010) == FreshnessVerdict::fresh);
  FO_REQUIRE(verdict_at(1011) == FreshnessVerdict::stale);
  FO_REQUIRE(verdict_at(1100) == FreshnessVerdict::stale);
  FO_REQUIRE(verdict_at(1101) == FreshnessVerdict::expired);
  // future_skew permits an observation to lead the evaluation instant by up to
  // five seconds; beyond that it is refused as future-dated.
  FO_REQUIRE(verdict_at(995) == FreshnessVerdict::fresh);
  FO_REQUIRE(verdict_at(994) == FreshnessVerdict::future_dated);
  FO_REQUIRE(verdict_at(900) == FreshnessVerdict::future_dated);
}

FO_TEST(freshness_unevaluable_cases) {
  const FreshnessWindow window;
  const FreshnessAssessment unset = assess_freshness(window, Timestamp{}, at_seconds(0), at_seconds(0));
  FO_REQUIRE(unset.verdict == FreshnessVerdict::unevaluable);
  FO_REQUIRE(unset.code == ReasonCode::indeterminate);

  const FreshnessAssessment backwards = assess_freshness(window, at_seconds(10), at_seconds(5), at_seconds(20));
  FO_REQUIRE(backwards.verdict == FreshnessVerdict::unevaluable);

  const FreshnessAssessment no_instant = assess_freshness(window, at_seconds(10), at_seconds(10), Timestamp{});
  FO_REQUIRE(no_instant.verdict == FreshnessVerdict::unevaluable);
}

FO_TEST(freshness_is_monotone_in_age) {
  FreshnessWindow window;
  window.fresh_for = std::chrono::seconds(60);
  window.stale_after = std::chrono::seconds(600);
  const Timestamp observed = at_seconds(10000);
  int previous = -1;
  bool saw_stale = false;
  bool saw_expired = false;
  for (std::int64_t age = 0; age <= 1200; ++age) {
    const FreshnessAssessment assessment = assess_freshness(window, observed, observed, at_seconds(10000 + age));
    const int rank = assessment.verdict == FreshnessVerdict::fresh     ? 0
                     : assessment.verdict == FreshnessVerdict::stale   ? 1
                     : assessment.verdict == FreshnessVerdict::expired ? 2
                                                                       : 3;
    FO_REQUIRE(rank >= previous);
    previous = rank;
    saw_stale = saw_stale || rank == 1;
    saw_expired = saw_expired || rank == 2;
  }
  FO_REQUIRE(saw_stale);
  FO_REQUIRE(saw_expired);
}

FO_TEST(freshness_window_validation) {
  FreshnessWindow bad;
  bad.fresh_for = std::chrono::seconds(10);
  bad.stale_after = std::chrono::seconds(5);
  FO_REQUIRE(!validate_freshness_window(bad).has_value());

  FreshnessWindow negative;
  negative.fresh_for = std::chrono::seconds(-1);
  FO_REQUIRE(!validate_freshness_window(negative).has_value());

  FreshnessWindow skew;
  skew.future_skew = std::chrono::seconds(-1);
  FO_REQUIRE(!validate_freshness_window(skew).has_value());
}

FO_TEST(policy_set_falls_back_and_overrides) {
  PolicySet policies;
  AspectPolicy fallback;
  fallback.freshness.fresh_for = std::chrono::seconds(11);
  FO_REQUIRE(policies.set_default(fallback).has_value());

  const AspectId draw = AspectId::parse("power.draw").value();
  FO_REQUIRE(policies.policy_for(draw).freshness.fresh_for == std::chrono::seconds(11));
  FO_REQUIRE(!policies.has_explicit_policy(draw));

  const AspectPolicy explicit_policy = make_policy("power.draw", 2, 3);
  FO_REQUIRE(policies.set(explicit_policy).has_value());
  FO_REQUIRE(policies.has_explicit_policy(draw));
  FO_REQUIRE(policies.policy_for(draw).freshness.fresh_for == std::chrono::seconds(2));
  FO_REQUIRE_EQ(policies.size(), 1);
  FO_REQUIRE_EQ(policies.explicit_policies().size(), 1);
}

FO_TEST(policy_validation) {
  const AspectPolicy empty;
  FO_REQUIRE(!validate_aspect_policy(empty).has_value());

  AspectPolicy bad_unit = make_policy("power.draw", 1, 2);
  bad_unit.canonical_unit = static_cast<Unit>(200);
  FO_REQUIRE(!validate_aspect_policy(bad_unit).has_value());

  AspectPolicy duplicated = make_policy("power.draw", 1, 2);
  duplicated.precedence.push_back(AuthorityId::parse("a").value());
  duplicated.precedence.push_back(AuthorityId::parse("a").value());
  FO_REQUIRE(!validate_aspect_policy(duplicated).has_value());

  const AspectPolicy bad_window = make_policy("power.draw", 10, 1);
  FO_REQUIRE(!validate_aspect_policy(bad_window).has_value());
}

FO_TEST(contract_boundary_matrix) {
  FO_REQUIRE(kind_may_publish_into(AuthorityKind::dccp, Domain::facility));
  FO_REQUIRE(kind_may_publish_into(AuthorityKind::dccp, Domain::incident));
  FO_REQUIRE(!kind_may_publish_into(AuthorityKind::dccp, Domain::asset));
  FO_REQUIRE(kind_may_publish_into(AuthorityKind::asi, Domain::asset));
  FO_REQUIRE(kind_may_publish_into(AuthorityKind::asi, Domain::dependency));
  FO_REQUIRE(!kind_may_publish_into(AuthorityKind::asi, Domain::incident));
  FO_REQUIRE(kind_may_publish_into(AuthorityKind::synthetic, Domain::incident));
  FO_REQUIRE(!kind_may_publish_into(AuthorityKind::unknown, Domain::facility));
}

FO_TEST(contract_envelope_round_trip_and_refusals) {
  EvidenceEnvelope envelope;
  envelope.authority = AuthorityId::parse("dccp-a").value();
  envelope.authority_kind = AuthorityKind::dccp;
  envelope.source = SourceId::parse("telemetry").value();
  envelope.epoch = Epoch::from_value(2);
  envelope.generation = Generation::from_value(5);
  envelope.revision = Revision::from_value(1);
  envelope.observed_at = at_seconds(100);
  envelope.published_at = at_seconds(101);
  envelope.entity = EntityRef::parse("rack:r1").value();
  envelope.aspect = AspectId::parse("power.draw").value();
  envelope.value = Value::make_scalar(FixedPoint::parse("4.25").value(), Unit::kilowatts).value();
  envelope.origin = "unit-test";

  auto decoded = envelope_from_json(envelope_to_json(envelope));
  FO_REQUIRE(decoded.has_value());
  FO_REQUIRE(decoded.value() == envelope);

  auto record = to_evidence(envelope, at_seconds(102));
  FO_REQUIRE(record.has_value());
  FO_REQUIRE_EQ(record.value().provenance.origin, std::string("unit-test"));
  FO_REQUIRE_EQ(record.value().provenance.received_at.to_rfc3339(), std::string("1970-01-01T00:01:42Z"));

  EvidenceEnvelope wrong_version = envelope;
  wrong_version.contract_version = 99;
  auto refused = to_evidence(wrong_version, at_seconds(1));
  FO_REQUIRE(!refused.has_value());
  FO_REQUIRE(refused.code() == ReasonCode::not_supported);

  EvidenceEnvelope wrong_domain = envelope;
  wrong_domain.entity = EntityRef::parse("asset:a1").value();
  auto boundary = to_evidence(wrong_domain, at_seconds(1));
  FO_REQUIRE(!boundary.has_value());
  FO_REQUIRE(boundary.code() == ReasonCode::authority_boundary_violation);

  EvidenceEnvelope unknown_kind = envelope;
  unknown_kind.authority_kind = AuthorityKind::unknown;
  FO_REQUIRE(!to_evidence(unknown_kind, at_seconds(1)).has_value());

  EvidenceEnvelope backwards = envelope;
  backwards.published_at = at_seconds(99);
  FO_REQUIRE(!to_evidence(backwards, at_seconds(1)).has_value());

  EvidenceEnvelope zero_generation = envelope;
  zero_generation.generation = Generation{};
  FO_REQUIRE(!to_evidence(zero_generation, at_seconds(1)).has_value());
}

FO_TEST(ingest_document_shapes) {
  auto authority_document = JsonValue::parse(
      "{\"kind\":\"authority\",\"id\":\"dccp-a\",\"authority_kind\":\"dccp\",\"role\":\"authority\",\"label\":\"a\","
      "\"epoch\":3}");
  FO_REQUIRE(authority_document.has_value());
  auto parsed_authority = ingest_document_from_json(authority_document.value());
  FO_REQUIRE(parsed_authority.has_value());
  FO_REQUIRE(parsed_authority.value().kind == IngestDocument::Kind::authority);
  FO_REQUIRE_EQ(parsed_authority.value().authority.epoch.value(), 3);

  auto epoch_document = JsonValue::parse(
      "{\"kind\":\"epoch\",\"authority\":\"dccp-a\",\"epoch\":4,\"published_at\":\"1970-01-01T00:00:10Z\"}");
  FO_REQUIRE(epoch_document.has_value());
  auto parsed_epoch = ingest_document_from_json(epoch_document.value());
  FO_REQUIRE(parsed_epoch.has_value());
  FO_REQUIRE_EQ(parsed_epoch.value().epoch.value(), 4);

  auto evidence_document = JsonValue::parse(
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"1970-01-01T00:01:40Z\","
      "\"published_at\":\"1970-01-01T00:01:40Z\",\"subject\":\"rack:r1\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"4.25\",\"unit\":\"kilowatts\"}}");
  FO_REQUIRE(evidence_document.has_value());
  auto parsed_evidence = ingest_document_from_json(evidence_document.value());
  FO_REQUIRE(parsed_evidence.has_value());
  FO_REQUIRE(parsed_evidence.value().kind == IngestDocument::Kind::evidence);

  auto unknown_kind = JsonValue::parse("{\"kind\":\"nonsense\"}");
  FO_REQUIRE(unknown_kind.has_value());
  FO_REQUIRE(!ingest_document_from_json(unknown_kind.value()).has_value());

  auto missing = JsonValue::parse("{\"kind\":\"authority\"}");
  FO_REQUIRE(missing.has_value());
  FO_REQUIRE(!ingest_document_from_json(missing.value()).has_value());
}
