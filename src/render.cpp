// Facility Observatory - shared JSON rendering of results.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/render.hpp"

#include <cstdint>
#include <string>

#include "facility_observatory/contracts.hpp"
#include "facility_observatory/version.hpp"

namespace fo {
namespace {

JsonValue render_unit(Unit unit) { return JsonValue::make_string(std::string(to_string(unit))); }

JsonValue render_timestamp(const Timestamp& stamp) {
  return JsonValue::make_string(stamp.is_set() ? stamp.to_rfc3339() : std::string("unset"));
}

}  // namespace

JsonValue render_version() {
  JsonValue object = JsonValue::make_object();
  object.set("library_version", JsonValue::make_string(std::string(version_string())));
  object.set("abi_tag", JsonValue::make_string(std::string(abi_tag())));
  object.set("journal_format_version",
             JsonValue::make_integer(static_cast<std::int64_t>(journal_format_version())));
  object.set("evidence_contract_version",
             JsonValue::make_integer(static_cast<std::int64_t>(evidence_contract_version())));
  object.set("systems_boundary", JsonValue::make_string(std::string(systems_boundary_statement())));
  return object;
}

JsonValue render_reason(const Reason& reason) {
  JsonValue object = JsonValue::make_object();
  object.set("code", JsonValue::make_string(std::string(to_string(reason.code))));
  object.set("detail", JsonValue::make_string(reason.detail));
  return object;
}

JsonValue render_value(const Value& value) {
  JsonValue object = JsonValue::make_object();
  object.set("kind", JsonValue::make_string(std::string(to_string(value.kind()))));
  switch (value.kind()) {
    case ValueKind::absent:
      break;
    case ValueKind::scalar:
      object.set("decimal", JsonValue::make_string(value.scalar_magnitude().to_string()));
      object.set("scale", JsonValue::make_integer(value.scalar_magnitude().scale()));
      object.set("unit", render_unit(value.unit()));
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
  object.set("rendered", JsonValue::make_string(value.to_string()));
  return object;
}

JsonValue render_evidence_ref(const EvidenceRef& reference) {
  JsonValue object = JsonValue::make_object();
  object.set("authority", JsonValue::make_string(reference.key.authority.str()));
  object.set("source", JsonValue::make_string(reference.key.source.str()));
  object.set("subject", JsonValue::make_string(reference.key.entity.to_string()));
  object.set("aspect", JsonValue::make_string(reference.key.aspect.str()));
  object.set("epoch", JsonValue::make_integer(static_cast<std::int64_t>(reference.epoch.value())));
  object.set("generation", JsonValue::make_integer(static_cast<std::int64_t>(reference.generation.value())));
  object.set("revision", JsonValue::make_integer(static_cast<std::int64_t>(reference.revision.value())));
  object.set("observed_at", render_timestamp(reference.observed_at));
  object.set("published_at", render_timestamp(reference.published_at));
  object.set("durability", JsonValue::make_string(std::string(to_string(reference.durability))));
  object.set("sequence", JsonValue::make_integer(static_cast<std::int64_t>(reference.sequence.value())));
  return object;
}

JsonValue render_evidence_record(const EvidenceRecord& record) {
  JsonValue object = render_evidence_ref(to_reference(record));
  object.set("value", render_value(record.value));
  object.set("explanation", JsonValue::make_string(record.explanation));
  object.set("origin", JsonValue::make_string(record.provenance.origin));
  return object;
}

JsonValue render_authority(const AuthorityDescriptor& descriptor) {
  JsonValue object = JsonValue::make_object();
  object.set("id", JsonValue::make_string(descriptor.id.str()));
  object.set("authority_kind", JsonValue::make_string(std::string(to_string(descriptor.kind))));
  object.set("role", JsonValue::make_string(std::string(to_string(descriptor.role))));
  object.set("label", JsonValue::make_string(descriptor.label.str()));
  object.set("epoch", JsonValue::make_integer(static_cast<std::int64_t>(descriptor.epoch.value())));
  object.set("epoch_published_at", render_timestamp(descriptor.epoch_published_at));
  object.set("synthetic", JsonValue::make_boolean(descriptor.synthetic));
  return object;
}

JsonValue render_evaluation(const Evaluation& evaluation) {
  JsonValue object = JsonValue::make_object();
  object.set("subject", JsonValue::make_string(evaluation.entity.to_string()));
  object.set("aspect", JsonValue::make_string(evaluation.aspect.str()));
  object.set("state", JsonValue::make_string(std::string(to_string(evaluation.state))));
  object.set("reason", render_reason(Reason{evaluation.code, evaluation.detail}));
  if (evaluation.value.has_value()) {
    object.set("value", render_value(evaluation.value.value()));
  } else {
    object.set("value", JsonValue::make_null());
  }
  JsonValue participants = JsonValue::make_array();
  for (const EvidenceRef& reference : evaluation.participants) {
    participants.push_back(render_evidence_ref(reference));
  }
  object.set("participants", std::move(participants));
  JsonValue contributors = JsonValue::make_array();
  for (const EvidenceRef& reference : evaluation.contributors) {
    contributors.push_back(render_evidence_ref(reference));
  }
  object.set("contributors", std::move(contributors));
  JsonValue disagreement = JsonValue::make_array();
  for (const ConflictingValue& claim : evaluation.disagreement) {
    JsonValue entry = JsonValue::make_object();
    entry.set("value", render_value(claim.value));
    entry.set("source", render_evidence_ref(claim.source));
    disagreement.push_back(std::move(entry));
  }
  object.set("disagreement", std::move(disagreement));
  JsonValue notes = JsonValue::make_array();
  for (const std::optional<Reason>& note : evaluation.participant_notes) {
    if (note.has_value()) {
      notes.push_back(render_reason(note.value()));
    } else {
      notes.push_back(JsonValue::make_null());
    }
  }
  object.set("participant_notes", std::move(notes));
  return object;
}

JsonValue render_aggregate(const AggregateResult& result) {
  JsonValue object = JsonValue::make_object();
  object.set("scope", JsonValue::make_string(result.scope.to_string()));
  object.set("aspect", JsonValue::make_string(result.aspect.str()));
  object.set("aggregation", JsonValue::make_string(std::string(to_string(result.kind))));
  object.set("state", JsonValue::make_string(std::string(to_string(result.state))));
  object.set("reason", render_reason(Reason{result.code, result.detail}));
  object.set("expected_contributors", JsonValue::make_integer(static_cast<std::int64_t>(result.expected_contributors)));
  object.set("observed_contributors", JsonValue::make_integer(static_cast<std::int64_t>(result.observed_contributors)));
  if (result.value.has_value()) {
    object.set("value", render_value(result.value.value()));
  } else {
    object.set("value", JsonValue::make_null());
  }
  JsonValue missing = JsonValue::make_array();
  for (const EntityRef& entity : result.missing) {
    missing.push_back(JsonValue::make_string(entity.to_string()));
  }
  object.set("missing", std::move(missing));
  JsonValue contributors = JsonValue::make_array();
  for (const EvidenceRef& reference : result.contributors) {
    contributors.push_back(render_evidence_ref(reference));
  }
  object.set("contributors", std::move(contributors));
  return object;
}

JsonValue render_divergence(const Divergence& divergence) {
  JsonValue object = JsonValue::make_object();
  object.set("class", JsonValue::make_string(std::string(to_string(divergence.divergence_class))));
  object.set("left_subject", JsonValue::make_string(divergence.left_entity.to_string()));
  object.set("right_subject", JsonValue::make_string(divergence.right_entity.to_string()));
  object.set("aspect", JsonValue::make_string(divergence.aspect.str()));
  object.set("left_state", JsonValue::make_string(std::string(to_string(divergence.left_state))));
  object.set("right_state", JsonValue::make_string(std::string(to_string(divergence.right_state))));
  if (divergence.left_value.has_value()) {
    object.set("left_value", render_value(divergence.left_value.value()));
  } else {
    object.set("left_value", JsonValue::make_null());
  }
  if (divergence.right_value.has_value()) {
    object.set("right_value", render_value(divergence.right_value.value()));
  } else {
    object.set("right_value", JsonValue::make_null());
  }
  if (divergence.delta.has_value()) {
    object.set("delta", JsonValue::make_string(divergence.delta.value().to_string()));
  } else {
    object.set("delta", JsonValue::make_null());
  }
  if (divergence.relative_delta.has_value()) {
    object.set("relative_delta", JsonValue::make_string(divergence.relative_delta.value().to_string()));
  } else {
    object.set("relative_delta", JsonValue::make_null());
  }
  object.set("unit", render_unit(divergence.unit));
  object.set("detail", JsonValue::make_string(divergence.detail));
  JsonValue left = JsonValue::make_array();
  for (const EvidenceRef& reference : divergence.left_evidence) {
    left.push_back(render_evidence_ref(reference));
  }
  object.set("left_evidence", std::move(left));
  JsonValue right = JsonValue::make_array();
  for (const EvidenceRef& reference : divergence.right_evidence) {
    right.push_back(render_evidence_ref(reference));
  }
  object.set("right_evidence", std::move(right));
  return object;
}

JsonValue render_recovery(const RecoveryReport& report) {
  JsonValue object = JsonValue::make_object();
  object.set("code", JsonValue::make_string(std::string(to_string(report.code))));
  object.set("detail", JsonValue::make_string(report.detail));
  object.set("header_valid", JsonValue::make_boolean(report.header_valid));
  object.set("clean", JsonValue::make_boolean(report.clean));
  object.set("created", JsonValue::make_boolean(report.created));
  object.set("torn_tail", JsonValue::make_boolean(report.torn_tail));
  object.set("interior_corruption", JsonValue::make_boolean(report.interior_corruption));
  object.set("truncated", JsonValue::make_boolean(report.truncated));
  object.set("format_version", JsonValue::make_integer(static_cast<std::int64_t>(report.format_version)));
  object.set("total_bytes", JsonValue::make_integer(static_cast<std::int64_t>(report.total_bytes)));
  object.set("valid_bytes", JsonValue::make_integer(static_cast<std::int64_t>(report.valid_bytes)));
  object.set("lost_bytes", JsonValue::make_integer(static_cast<std::int64_t>(report.lost_bytes)));
  object.set("record_count", JsonValue::make_integer(static_cast<std::int64_t>(report.record_count)));
  object.set("created_at", render_timestamp(report.created_at));
  return object;
}

JsonValue render_status(const ObservatoryStatus& status) {
  JsonValue object = JsonValue::make_object();
  object.set("open", JsonValue::make_boolean(status.open));
  object.set("journal_path", JsonValue::make_string(status.journal_path));
  object.set("records", JsonValue::make_integer(static_cast<std::int64_t>(status.records)));
  object.set("authorities", JsonValue::make_integer(static_cast<std::int64_t>(status.authorities)));
  object.set("subjects", JsonValue::make_integer(static_cast<std::int64_t>(status.subjects)));
  object.set("policies", JsonValue::make_integer(static_cast<std::int64_t>(status.policies)));
  object.set("journal_records", JsonValue::make_integer(static_cast<std::int64_t>(status.journal_records)));
  object.set("journal_bytes", JsonValue::make_integer(static_cast<std::int64_t>(status.journal_bytes)));
  object.set("digest", JsonValue::make_string(status.digest));
  object.set("lock_order_violations",
             JsonValue::make_integer(static_cast<std::int64_t>(status.lock_order_violations)));
  object.set("recovery", render_recovery(status.recovery));
  return object;
}

JsonValue render_snapshot(const Snapshot& snapshot) {
  JsonValue object = JsonValue::make_object();
  object.set("sequence", JsonValue::make_integer(static_cast<std::int64_t>(snapshot.sequence.value())));
  object.set("created_at", render_timestamp(snapshot.created_at));
  object.set("digest", JsonValue::make_string(snapshot.digest));
  object.set("records", JsonValue::make_integer(static_cast<std::int64_t>(snapshot.record_count)));
  object.set("authorities", JsonValue::make_integer(static_cast<std::int64_t>(snapshot.authority_count)));
  object.set("subjects", JsonValue::make_integer(static_cast<std::int64_t>(snapshot.subject_count)));
  object.set("policies", JsonValue::make_integer(static_cast<std::int64_t>(snapshot.policy_count)));
  return object;
}

JsonValue render_ingestion(const IngestionOutcome& outcome) {
  JsonValue object = JsonValue::make_object();
  object.set("code", JsonValue::make_string(std::string(to_string(outcome.code))));
  object.set("detail", JsonValue::make_string(outcome.detail));
  object.set("durable", JsonValue::make_boolean(outcome.durable));
  object.set("sequence", JsonValue::make_integer(static_cast<std::int64_t>(outcome.sequence.value())));
  JsonValue admission = JsonValue::make_object();
  admission.set("code", JsonValue::make_string(std::string(to_string(outcome.admission.code))));
  admission.set("admitted", JsonValue::make_boolean(outcome.admission.admitted));
  admission.set("retained", JsonValue::make_boolean(outcome.admission.retained));
  admission.set("duplicate", JsonValue::make_boolean(outcome.admission.duplicate));
  admission.set("fence_advanced", JsonValue::make_boolean(outcome.admission.fence_advanced));
  admission.set("effective_epoch",
                JsonValue::make_integer(static_cast<std::int64_t>(outcome.admission.effective_epoch.value())));
  admission.set("effective_generation",
                JsonValue::make_integer(static_cast<std::int64_t>(outcome.admission.effective_generation.value())));
  admission.set("effective_revision",
                JsonValue::make_integer(static_cast<std::int64_t>(outcome.admission.effective_revision.value())));
  object.set("admission", std::move(admission));
  return object;
}

}  // namespace fo
