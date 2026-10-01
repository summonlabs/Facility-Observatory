// Facility Observatory - shared JSON rendering of results.
//
// Every public result has exactly one rendering, used by the CLI, by tests and
// by downstream consumers, so documentation and behaviour cannot drift apart.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_RENDER_HPP
#define FACILITY_OBSERVATORY_RENDER_HPP

#include "facility_observatory/authority.hpp"
#include "facility_observatory/divergence.hpp"
#include "facility_observatory/evidence.hpp"
#include "facility_observatory/export.hpp"
#include "facility_observatory/json.hpp"
#include "facility_observatory/observatory.hpp"

namespace fo {

FO_API JsonValue render_version();
FO_API JsonValue render_reason(const Reason& reason);
FO_API JsonValue render_value(const Value& value);
FO_API JsonValue render_evidence_ref(const EvidenceRef& reference);
FO_API JsonValue render_evidence_record(const EvidenceRecord& record);
FO_API JsonValue render_authority(const AuthorityDescriptor& descriptor);
FO_API JsonValue render_evaluation(const Evaluation& evaluation);
FO_API JsonValue render_aggregate(const AggregateResult& result);
FO_API JsonValue render_divergence(const Divergence& divergence);
FO_API JsonValue render_recovery(const RecoveryReport& report);
FO_API JsonValue render_status(const ObservatoryStatus& status);
FO_API JsonValue render_snapshot(const Snapshot& snapshot);
FO_API JsonValue render_ingestion(const IngestionOutcome& outcome);

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_RENDER_HPP
