// Facility Observatory - shared test fixtures.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_TESTS_FIXTURES_HPP
#define FACILITY_OBSERVATORY_TESTS_FIXTURES_HPP

#include <chrono>
#include <cstdint>
#include <string>

#include "facility_observatory/authority.hpp"
#include "facility_observatory/divergence.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/policy.hpp"
#include "facility_observatory/store.hpp"
#include "facility_observatory/time.hpp"

namespace fotest {

inline fo::Timestamp at_seconds(std::int64_t seconds) {
  return fo::Timestamp::from_unix_nanos(seconds * 1000000000LL).value();
}

inline fo::AuthorityDescriptor make_authority(const char* id, fo::AuthorityKind kind, std::uint64_t epoch = 1) {
  fo::AuthorityDescriptor descriptor;
  descriptor.id = fo::AuthorityId::parse(id).value();
  descriptor.kind = kind;
  descriptor.role = fo::AuthorityRole::authority;
  descriptor.label = fo::Token::parse(id).value();
  descriptor.epoch = fo::Epoch::from_value(epoch);
  descriptor.epoch_published_at = at_seconds(0);
  return descriptor;
}

inline fo::EvidenceRecord make_evidence(const char* authority, const char* source_kind, const char* subject,
                                        const char* aspect, std::uint64_t epoch, std::uint64_t generation,
                                        std::uint64_t revision, const char* decimal, const char* unit,
                                        std::int64_t observed_seconds) {
  fo::EvidenceRecord record;
  record.key.authority = fo::AuthorityId::parse(authority).value();
  record.key.source = fo::SourceId::parse(source_kind).value();
  record.key.entity = fo::EntityRef::parse(subject).value();
  record.key.aspect = fo::AspectId::parse(aspect).value();
  record.epoch = fo::Epoch::from_value(epoch);
  record.generation = fo::Generation::from_value(generation);
  record.revision = fo::Revision::from_value(revision);
  record.observed_at = at_seconds(observed_seconds);
  record.published_at = at_seconds(observed_seconds);
  record.value =
      fo::Value::make_scalar(fo::FixedPoint::parse(decimal).value(), fo::unit_from_string(unit).value()).value();
  record.provenance.received_at = record.published_at;
  record.provenance.epoch_at_receipt = record.epoch;
  record.provenance.origin = "fixture";
  return record;
}

inline fo::EvidenceRecord make_text_evidence(const char* authority, const char* subject, const char* aspect,
                                             std::uint64_t generation, const char* text,
                                             std::int64_t observed_seconds) {
  fo::EvidenceRecord record;
  record.key.authority = fo::AuthorityId::parse(authority).value();
  record.key.source = fo::SourceId::parse("topology").value();
  record.key.entity = fo::EntityRef::parse(subject).value();
  record.key.aspect = fo::AspectId::parse(aspect).value();
  record.epoch = fo::Epoch::from_value(1);
  record.generation = fo::Generation::from_value(generation);
  record.revision = fo::Revision::from_value(1);
  record.observed_at = at_seconds(observed_seconds);
  record.published_at = at_seconds(observed_seconds);
  record.value = fo::Value::make_text(text).value();
  record.provenance.received_at = record.published_at;
  record.provenance.origin = "fixture";
  return record;
}

inline fo::AspectPolicy make_policy(const char* aspect, std::int64_t fresh_seconds, std::int64_t stale_seconds,
                                    fo::AspectClass aspect_class = fo::AspectClass::dynamic_measurement) {
  fo::AspectPolicy policy;
  policy.aspect = fo::AspectId::parse(aspect).value();
  policy.freshness.fresh_for = std::chrono::seconds(fresh_seconds);
  policy.freshness.stale_after = std::chrono::seconds(stale_seconds);
  policy.aspect_class = aspect_class;
  return policy;
}

}  // namespace fotest

#endif  // FACILITY_OBSERVATORY_TESTS_FIXTURES_HPP
