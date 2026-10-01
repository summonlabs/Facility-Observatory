// Facility Observatory - divergence detection tests.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <chrono>
#include <string>
#include <vector>

#include "facility_observatory/divergence.hpp"
#include "facility_observatory/store.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

TopologyConventions topology() {
  return TopologyConventions::make("topology.parent", "power.total", "power.draw").value();
}

DivergenceQuery query() { return DivergenceQuery::make_default().value(); }

void seed_site(EvidenceStore& store) {
  FO_REQUIRE(store.register_authority(make_authority("dccp-a", AuthorityKind::dccp)).has_value());
  FO_REQUIRE(store.register_authority(make_authority("bms-a", AuthorityKind::bms)).has_value());
  FO_REQUIRE(store.admit(make_text_evidence("dccp-a", "rack:r1", "topology.parent", 1, "site:sea1", 100)).retained);
  FO_REQUIRE(store.admit(make_text_evidence("dccp-a", "rack:r2", "topology.parent", 1, "site:sea1", 100)).retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "1500", "watts", 100))
                  .retained);
  FO_REQUIRE(store.admit(make_evidence("dccp-a", "telemetry", "site:sea1", "power.total", 1, 1, 1, "3", "kilowatts", 100))
                  .retained);
}

const Divergence* find_class(const std::vector<Divergence>& results, DivergenceClass wanted) {
  for (const Divergence& divergence : results) {
    if (divergence.divergence_class == wanted) {
      return &divergence;
    }
  }
  return nullptr;
}

}  // namespace

FO_TEST(divergence_source_disagreement) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1.6", "kilowatts", 100))
                  .retained);

  DivergenceDetector detector(store, topology(), query());
  auto found = detector.detect(at_seconds(100));
  FO_REQUIRE(found.has_value());

  const Divergence* disagreement = find_class(found.value(), DivergenceClass::source_disagreement);
  FO_REQUIRE(disagreement != nullptr);
  FO_REQUIRE(disagreement->left_value.has_value());
  FO_REQUIRE(disagreement->right_value.has_value());
  FO_REQUIRE(disagreement->delta.has_value());
  FO_REQUIRE_EQ(disagreement->delta.value().to_string(), std::string("-100"));
  FO_REQUIRE(disagreement->unit == Unit::watts);
  FO_REQUIRE(disagreement->relative_delta.has_value());
  FO_REQUIRE(disagreement->detail.find("bms-a") != std::string::npos);
}

FO_TEST(divergence_aggregation_mismatch_and_tolerance) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store
                 .admit(make_evidence("dccp-a", "telemetry", "site:sea1", "power.total", 1, 2, 1, "3.5", "kilowatts", 100))
                 .retained);
  DivergenceDetector detector(store, topology(), query());
  auto found = detector.detect(at_seconds(100));
  FO_REQUIRE(found.has_value());

  const Divergence* mismatch = find_class(found.value(), DivergenceClass::aggregation_mismatch);
  FO_REQUIRE(mismatch != nullptr);
  FO_REQUIRE_EQ(mismatch->left_value.value().to_string(), std::string("3500 watts"));
  FO_REQUIRE_EQ(mismatch->right_value.value().to_string(), std::string("3000 watts"));
  FO_REQUIRE(mismatch->delta.has_value());
  FO_REQUIRE_EQ(mismatch->delta.value().to_string(), std::string("-500"));
  FO_REQUIRE(mismatch->unit == Unit::watts);

  EvidenceStore tolerant;
  seed_site(tolerant);
  FO_REQUIRE(tolerant
                 .admit(make_evidence("dccp-a", "telemetry", "site:sea1", "power.total", 1, 2, 1, "3.001", "kilowatts",
                                      100))
                 .retained);
  DivergenceQuery loose = query();
  loose.relative_tolerance = FixedPoint::parse("0.01").value();
  DivergenceDetector tolerant_detector(tolerant, topology(), loose);
  auto tolerant_found = tolerant_detector.detect(at_seconds(100));
  FO_REQUIRE(tolerant_found.has_value());
  FO_REQUIRE(find_class(tolerant_found.value(), DivergenceClass::aggregation_mismatch) == nullptr);
}

FO_TEST(divergence_temporal_skew) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1500", "watts", 900))
                  .retained);

  DivergenceQuery settings = query();
  settings.temporal_skew_threshold = std::chrono::seconds(10);
  DivergenceDetector detector(store, topology(), settings);
  auto found = detector.detect(at_seconds(900));
  FO_REQUIRE(found.has_value());
  const Divergence* skew = find_class(found.value(), DivergenceClass::temporal_skew);
  FO_REQUIRE(skew != nullptr);
  FO_REQUIRE_EQ(skew->left_evidence.size(), 1);
  FO_REQUIRE_EQ(skew->right_evidence.size(), 1);
}

FO_TEST(divergence_coverage_gap) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_text_evidence("dccp-a", "rack:r3", "topology.parent", 1, "site:sea1", 100)).retained);

  DivergenceDetector detector(store, topology(), query());
  auto found = detector.detect(at_seconds(100));
  FO_REQUIRE(found.has_value());
  const Divergence* gap = find_class(found.value(), DivergenceClass::coverage_gap);
  FO_REQUIRE(gap != nullptr);
  FO_REQUIRE_EQ(gap->right_entity.to_string(), std::string("rack:r3"));
}

FO_TEST(divergence_unit_incompatible) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "25", "celsius", 100))
                  .retained);
  DivergenceDetector detector(store, topology(), query());
  auto found = detector.detect(at_seconds(100));
  FO_REQUIRE(found.has_value());
  FO_REQUIRE(find_class(found.value(), DivergenceClass::unit_incompatible) != nullptr);
}

FO_TEST(divergence_recovered_versus_live) {
  EvidenceStore store;
  seed_site(store);

  EvidenceRecord recovered =
      make_evidence("bms-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "1.4", "kilowatts", 100);
  recovered.provenance.durability = Durability::recovered;
  FO_REQUIRE(store.admit(recovered).retained);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r2", "power.draw", 1, 1, 1, "1.9", "kilowatts", 100))
                  .retained);

  DivergenceDetector detector(store, topology(), query());
  auto found = detector.detect(at_seconds(100));
  FO_REQUIRE(found.has_value());
  const Divergence* mismatch = find_class(found.value(), DivergenceClass::recovered_versus_live);
  FO_REQUIRE(mismatch != nullptr);
  FO_REQUIRE(mismatch->detail.find("durable history holds") != std::string::npos);
}

FO_TEST(divergence_results_are_canonical_and_bounded) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1.6", "kilowatts", 100))
                  .retained);
  DivergenceDetector detector(store, topology(), query());

  auto first = detector.detect(at_seconds(100));
  auto second = detector.detect(at_seconds(100));
  FO_REQUIRE(first.has_value());
  FO_REQUIRE(second.has_value());
  FO_REQUIRE_EQ(first.value().size(), second.value().size());
  for (std::size_t index = 0; index < first.value().size(); ++index) {
    FO_REQUIRE(first.value()[index] == second.value()[index]);
  }
  for (std::size_t index = 1; index < first.value().size(); ++index) {
    FO_REQUIRE(!divergence_less(first.value()[index], first.value()[index - 1]));
  }

  DivergenceQuery tight = query();
  tight.max_divergences = 0;
  DivergenceDetector bounded(store, topology(), tight);
  auto exceeded = bounded.detect(at_seconds(100));
  FO_REQUIRE(!exceeded.has_value());
  FO_REQUIRE(exceeded.code() == ReasonCode::limit_exceeded);
}

FO_TEST(divergence_scoped_query_matches_global_subset) {
  EvidenceStore store;
  seed_site(store);
  FO_REQUIRE(store.admit(make_evidence("bms-a", "telemetry", "rack:r1", "power.draw", 1, 1, 1, "1.6", "kilowatts", 100))
                  .retained);
  DivergenceDetector detector(store, topology(), query());

  auto global = detector.detect(at_seconds(100));
  auto scoped = detector.detect_for(EntityRef::parse("rack:r1").value(), at_seconds(100));
  FO_REQUIRE(global.has_value());
  FO_REQUIRE(scoped.has_value());
  FO_REQUIRE(!scoped.value().empty());
  for (const Divergence& divergence : scoped.value()) {
    FO_REQUIRE(std::find(global.value().begin(), global.value().end(), divergence) != global.value().end());
  }

  auto missing = detector.detect(Timestamp{});
  FO_REQUIRE(!missing.has_value());
  FO_REQUIRE(missing.code() == ReasonCode::indeterminate);
}

FO_TEST(divergence_rejects_invalid_topology_conventions) {
  FO_REQUIRE(!TopologyConventions::make("Bad Aspect", "power.total", "power.draw").has_value());
  FO_REQUIRE(!TopologyConventions::make("topology.parent", "", "power.draw").has_value());
  FO_REQUIRE(!divergence_class_from_string("nonsense").has_value());
  FO_REQUIRE(divergence_class_from_string("temporal_skew").value() == DivergenceClass::temporal_skew);
}
