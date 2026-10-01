// Facility Observatory - freshness, aspect classification and view policy.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/policy.hpp"

#include <set>

namespace fo {

Status validate_freshness_window(const FreshnessWindow& window) {
  if (window.fresh_for.count() < 0) {
    return Status::fail(ReasonCode::invalid_argument, "fresh_for must not be negative");
  }
  if (window.stale_after.count() < 0) {
    return Status::fail(ReasonCode::invalid_argument, "stale_after must not be negative");
  }
  if (window.stale_after < window.fresh_for) {
    return Status::fail(ReasonCode::invalid_argument, "stale_after must not be shorter than fresh_for");
  }
  if (window.future_skew.count() < 0) {
    return Status::fail(ReasonCode::invalid_argument, "future_skew must not be negative");
  }
  return success();
}

std::string_view to_string(FreshnessVerdict verdict) noexcept {
  switch (verdict) {
    case FreshnessVerdict::fresh:
      return std::string_view{"fresh"};
    case FreshnessVerdict::stale:
      return std::string_view{"stale"};
    case FreshnessVerdict::expired:
      return std::string_view{"expired"};
    case FreshnessVerdict::future_dated:
      return std::string_view{"future_dated"};
    case FreshnessVerdict::unevaluable:
      return std::string_view{"unevaluable"};
  }
  return std::string_view{"unknown"};
}

FreshnessAssessment assess_freshness(const FreshnessWindow& window, Timestamp observed_at,
                                     Timestamp published_at, Timestamp at) {
  FreshnessAssessment assessment;
  if (!observed_at.is_set() || !published_at.is_set() || !at.is_set()) {
    assessment.verdict = FreshnessVerdict::unevaluable;
    assessment.code = ReasonCode::indeterminate;
    assessment.detail = "observation, publication and evaluation instants must all be set";
    return assessment;
  }
  if (published_at < observed_at) {
    assessment.verdict = FreshnessVerdict::unevaluable;
    assessment.code = ReasonCode::indeterminate;
    assessment.detail = "publication instant precedes observation instant";
    return assessment;
  }
  auto observation_age = duration_between(at, observed_at);
  if (!observation_age) {
    assessment.verdict = FreshnessVerdict::unevaluable;
    assessment.code = observation_age.code();
    assessment.detail = "observation age is not representable";
    return assessment;
  }
  auto publication_age = duration_between(at, published_at);
  if (!publication_age) {
    assessment.verdict = FreshnessVerdict::unevaluable;
    assessment.code = publication_age.code();
    assessment.detail = "publication age is not representable";
    return assessment;
  }
  assessment.ages_available = true;
  assessment.observation_age = observation_age.value();
  assessment.publication_age = publication_age.value();

  if (assessment.observation_age < Duration::zero()) {
    const Duration lead = -assessment.observation_age;
    if (lead > window.future_skew) {
      assessment.verdict = FreshnessVerdict::future_dated;
      assessment.code = ReasonCode::indeterminate;
      assessment.detail = "observation instant leads the evaluation instant by " + to_string(lead) +
                          ", beyond the permitted skew of " + to_string(window.future_skew);
      return assessment;
    }
    assessment.observation_age = Duration::zero();
  }
  if (assessment.publication_age < Duration::zero()) {
    assessment.publication_age = Duration::zero();
  }

  if (assessment.observation_age <= window.fresh_for) {
    assessment.verdict = FreshnessVerdict::fresh;
    assessment.code = ReasonCode::ok;
    assessment.detail = "observation age " + to_string(assessment.observation_age) + " is within fresh_for " +
                        to_string(window.fresh_for);
    return assessment;
  }
  if (assessment.observation_age <= window.stale_after) {
    assessment.verdict = FreshnessVerdict::stale;
    assessment.code = ReasonCode::stale_evidence;
    assessment.detail = "observation age " + to_string(assessment.observation_age) +
                        " exceeds fresh_for " + to_string(window.fresh_for) + " but is within stale_after " +
                        to_string(window.stale_after);
    return assessment;
  }
  assessment.verdict = FreshnessVerdict::expired;
  assessment.code = ReasonCode::expired_evidence;
  assessment.detail = "observation age " + to_string(assessment.observation_age) + " exceeds stale_after " +
                      to_string(window.stale_after);
  return assessment;
}

std::string_view to_string(AspectClass aspect_class) noexcept {
  switch (aspect_class) {
    case AspectClass::static_identity:
      return std::string_view{"static_identity"};
    case AspectClass::dynamic_measurement:
      return std::string_view{"dynamic_measurement"};
  }
  return std::string_view{"unknown"};
}

Outcome<AspectClass> aspect_class_from_string(std::string_view text) {
  if (text == "static_identity") {
    return Outcome<AspectClass>::ok(AspectClass::static_identity);
  }
  if (text == "dynamic_measurement") {
    return Outcome<AspectClass>::ok(AspectClass::dynamic_measurement);
  }
  return Outcome<AspectClass>::fail(ReasonCode::invalid_argument,
                                    "unknown aspect class '" + std::string(text) + "'");
}

Status validate_aspect_policy(const AspectPolicy& policy) {
  if (policy.aspect.empty()) {
    return Status::fail(ReasonCode::empty_input, "aspect policy must name an aspect");
  }
  auto window = validate_freshness_window(policy.freshness);
  if (!window) {
    return window;
  }
  if (policy.canonical_unit.has_value() && !is_known_unit(policy.canonical_unit.value())) {
    return Status::fail(ReasonCode::unit_incompatible, "aspect policy names an unknown canonical unit");
  }
  std::set<std::string> seen;
  for (const AuthorityId& id : policy.precedence) {
    if (id.empty()) {
      return Status::fail(ReasonCode::empty_input, "authority precedence list contains an empty identifier");
    }
    if (!seen.insert(id.str()).second) {
      return Status::fail(ReasonCode::invalid_argument,
                          "authority precedence list repeats '" + id.str() + "'");
    }
  }
  return success();
}

namespace {

// The default policy has no aspect of its own, so it is validated separately
// from a named aspect policy.
Status validate_default_policy(const AspectPolicy& policy) {
  if (policy.canonical_unit.has_value() && !is_known_unit(policy.canonical_unit.value())) {
    return Status::fail(ReasonCode::unit_incompatible, "default policy names an unknown canonical unit");
  }
  auto window = validate_freshness_window(policy.freshness);
  if (!window) {
    return window;
  }
  std::set<std::string> seen;
  for (const AuthorityId& id : policy.precedence) {
    if (id.empty()) {
      return Status::fail(ReasonCode::empty_input, "authority precedence list contains an empty identifier");
    }
    if (!seen.insert(id.str()).second) {
      return Status::fail(ReasonCode::invalid_argument, "authority precedence list repeats '" + id.str() + "'");
    }
  }
  return success();
}

}  // namespace

PolicySet::PolicySet() = default;

Status PolicySet::plan_default(const AspectPolicy& policy) const { return validate_default_policy(policy); }

Status PolicySet::plan(const AspectPolicy& policy) const { return validate_aspect_policy(policy); }

Status PolicySet::set_default(const AspectPolicy& policy) {
  auto validated = validate_default_policy(policy);
  if (!validated) {
    return validated;
  }
  default_policy_ = policy;
  return success();
}

Status PolicySet::set(const AspectPolicy& policy) {
  auto validated = validate_aspect_policy(policy);
  if (!validated) {
    return validated;
  }
  policies_[policy.aspect] = policy;
  return success();
}

bool PolicySet::has_explicit_policy(const AspectId& aspect) const {
  return policies_.find(aspect) != policies_.end();
}

const AspectPolicy& PolicySet::policy_for(const AspectId& aspect) const {
  const auto found = policies_.find(aspect);
  if (found != policies_.end()) {
    return found->second;
  }
  return default_policy_;
}

std::vector<AspectPolicy> PolicySet::explicit_policies() const {
  std::vector<AspectPolicy> result;
  result.reserve(policies_.size());
  for (const auto& entry : policies_) {
    result.push_back(entry.second);
  }
  return result;
}

void PolicySet::clear() { policies_.clear(); }

}  // namespace fo
