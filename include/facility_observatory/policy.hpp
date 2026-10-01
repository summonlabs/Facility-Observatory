// Facility Observatory - freshness, aspect classification and view policy.
//
// Policy is configuration supplied by the operator. It never changes what an
// authority published; it only changes how the observatory classifies and
// presents it.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_POLICY_HPP
#define FACILITY_OBSERVATORY_POLICY_HPP

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/export.hpp"
#include "facility_observatory/outcome.hpp"
#include "facility_observatory/strong.hpp"
#include "facility_observatory/time.hpp"

namespace fo {

// ---------------------------------------------------------------------------
// Freshness
// ---------------------------------------------------------------------------
struct FO_API FreshnessWindow {
  // An observation younger than this is fresh.
  Duration fresh_for{std::chrono::minutes(5)};
  // An observation older than fresh_for but at most this old is stale.
  // Anything older is expired.
  Duration stale_after{std::chrono::minutes(30)};
  // How far an authority's observation timestamp may lead the evaluation
  // instant before the evidence is refused as future-dated.
  Duration future_skew{std::chrono::seconds(0)};

  friend bool operator==(const FreshnessWindow&, const FreshnessWindow&) = default;
};

FO_API Status validate_freshness_window(const FreshnessWindow& window);

enum class FreshnessVerdict : std::uint8_t {
  fresh = 0,
  stale = 1,
  expired = 2,
  future_dated = 3,
  unevaluable = 4,
};

FO_API std::string_view to_string(FreshnessVerdict verdict) noexcept;

struct FO_API FreshnessAssessment {
  FreshnessVerdict verdict{FreshnessVerdict::unevaluable};
  ReasonCode code{ReasonCode::indeterminate};
  std::string detail{};
  Duration observation_age{0};
  Duration publication_age{0};
  bool ages_available{false};
};

// Freshness is judged on how old the observation is, not on when we happened to
// receive it: an authority that stops observing produces stale evidence even if
// it keeps republishing the same reading.
FO_API FreshnessAssessment assess_freshness(const FreshnessWindow& window, Timestamp observed_at,
                                            Timestamp published_at, Timestamp at);

// ---------------------------------------------------------------------------
// Aspect classification
// ---------------------------------------------------------------------------
enum class AspectClass : std::uint8_t {
  // Identity, topology and configuration. Does not decay with time, so evidence
  // recovered from durable storage may be re-promoted to current.
  static_identity = 0,
  // Telemetry and measurements. Decays with time, so recovered evidence stays
  // historical until a live publication refreshes it.
  dynamic_measurement = 1,
};

FO_API std::string_view to_string(AspectClass aspect_class) noexcept;
FO_API Outcome<AspectClass> aspect_class_from_string(std::string_view text);

// ---------------------------------------------------------------------------
// Aspect policy
// ---------------------------------------------------------------------------
struct FO_API AspectPolicy {
  AspectId aspect{};
  FreshnessWindow freshness{};
  AspectClass aspect_class{AspectClass::dynamic_measurement};
  // When set, scalar values are reported in this unit. When unset, the canonical
  // unit of the participating evidence is used.
  std::optional<Unit> canonical_unit{};
  // Ordered authority preference. When non-empty, the highest-ranked authority
  // that published current evidence determines the value, and any disagreement
  // is still reported. When empty, disagreement yields the conflicting state
  // rather than a silently chosen winner.
  std::vector<AuthorityId> precedence{};

  friend bool operator==(const AspectPolicy&, const AspectPolicy&) = default;
};

FO_API Status validate_aspect_policy(const AspectPolicy& policy);

class FO_API PolicySet {
 public:
  PolicySet();

  Status set_default(const AspectPolicy& policy);
  Status set(const AspectPolicy& policy);

  // Validation without mutation, so a caller can obtain a durable commit before
  // the in-memory policy changes.
  [[nodiscard]] Status plan_default(const AspectPolicy& policy) const;
  [[nodiscard]] Status plan(const AspectPolicy& policy) const;

  [[nodiscard]] const AspectPolicy& default_policy() const noexcept { return default_policy_; }
  [[nodiscard]] bool has_explicit_policy(const AspectId& aspect) const;
  [[nodiscard]] const AspectPolicy& policy_for(const AspectId& aspect) const;
  [[nodiscard]] std::vector<AspectPolicy> explicit_policies() const;
  [[nodiscard]] std::size_t size() const noexcept { return policies_.size(); }
  void clear();

 private:
  AspectPolicy default_policy_{};
  std::map<AspectId, AspectPolicy> policies_{};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_POLICY_HPP
