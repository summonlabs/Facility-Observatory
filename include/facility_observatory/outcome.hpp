// Facility Observatory - deterministic outcome and reason model.
//
// Every public API in this library returns either a value or an explicit,
// machine-readable Reason. Silence is never a result: "no evidence" is a
// distinct, reported outcome from "evidence says zero".
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_OUTCOME_HPP
#define FACILITY_OBSERVATORY_OUTCOME_HPP

#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "facility_observatory/export.hpp"

namespace fo {

// ---------------------------------------------------------------------------
// ReasonCode
//
// Stable numeric codes are part of the public contract: callers switch on them
// and CLI exit codes derive from them. Existing codes must never be renumbered.
// ---------------------------------------------------------------------------
enum class ReasonCode : std::uint16_t {
  ok = 0,

  // --- shape / encoding ---------------------------------------------------
  invalid_identifier = 10,
  identifier_too_long = 11,
  invalid_aspect_path = 12,
  malformed_encoding = 13,
  empty_input = 14,
  value_out_of_range = 15,
  arithmetic_overflow = 16,
  division_by_zero = 17,
  scale_mismatch = 18,
  inexact_conversion = 19,
  too_many_records = 20,
  record_too_large = 21,

  // --- authority / boundary ----------------------------------------------
  authority_unknown = 40,
  authority_boundary_violation = 41,
  authority_role_not_observer = 42,
  source_not_registered = 43,
  source_disappeared = 44,
  mutation_refused = 45,
  stale_authority_race = 46,

  // --- evidence semantics -------------------------------------------------
  no_evidence = 70,
  evidence_not_found = 71,
  stale_epoch = 72,
  replayed_generation = 73,
  duplicate_evidence = 74,
  superseded_revision = 75,
  unsupported_aspect = 76,
  conflicting_evidence = 77,
  unit_incompatible = 78,
  recovered_not_current = 79,
  indeterminate = 80,
  stale_evidence = 81,
  expired_evidence = 82,

  // --- persistence --------------------------------------------------------
  journal_missing = 100,
  journal_version_unsupported = 101,
  journal_header_corrupt = 102,
  journal_interior_corruption = 103,
  journal_torn_tail = 104,
  journal_locked = 105,
  journal_io_error = 106,
  journal_already_open = 107,
  journal_not_open = 108,
  journal_path_invalid = 109,
  journal_commit_failed = 110,
  recovery_refused = 111,
  lock_unavailable = 112,

  // --- query / lifecycle --------------------------------------------------
  unknown_entity = 140,
  unknown_domain = 141,
  unsupported_query = 142,
  limit_exceeded = 143,
  cancelled = 144,
  shutting_down = 145,
  internal_error = 146,
  invalid_argument = 147,
  not_supported = 148,
  bucket_overflow = 149,
};

// Stable, lower-case, machine-readable spelling of a reason code.
FO_API std::string_view to_string(ReasonCode code) noexcept;

// Parse the spelling produced by to_string(). Returns false when unknown.
FO_API bool reason_code_from_string(std::string_view text, ReasonCode& out) noexcept;

// True when the code is the success code.
FO_API constexpr bool is_ok(ReasonCode code) noexcept { return code == ReasonCode::ok; }

// ---------------------------------------------------------------------------
// Reason
// ---------------------------------------------------------------------------
struct Reason {
  ReasonCode code{ReasonCode::internal_error};
  std::string detail{};

  Reason() = default;
  Reason(ReasonCode c, std::string d) : code(c), detail(std::move(d)) {}

  friend bool operator==(const Reason& lhs, const Reason& rhs) noexcept {
    return lhs.code == rhs.code && lhs.detail == rhs.detail;
  }
};

FO_API std::string to_debug_string(const Reason& reason);

// ---------------------------------------------------------------------------
// Outcome<T>
//
// Value-or-reason. Deliberately not an exception channel: failures carry data.
// ---------------------------------------------------------------------------
struct Nothing {
  friend constexpr bool operator==(Nothing, Nothing) noexcept { return true; }
};

template <class T>
class [[nodiscard]] Outcome {
 public:
  using value_type = T;

  Outcome(T value) : storage_(std::in_place_index<kValueIndex>, std::move(value)) {}  // NOLINT(google-explicit-constructor)

  Outcome(const Outcome&) = default;
  Outcome(Outcome&&) noexcept = default;
  Outcome& operator=(const Outcome&) = default;
  Outcome& operator=(Outcome&&) noexcept = default;
  ~Outcome() = default;

  static Outcome ok(T value) { return Outcome(std::move(value)); }

  static Outcome fail(ReasonCode code, std::string detail = std::string{}) {
    return Outcome(Reason{code, std::move(detail)});
  }

  static Outcome fail(Reason reason) { return Outcome(std::move(reason)); }

  [[nodiscard]] bool has_value() const noexcept { return storage_.index() == kValueIndex; }
  [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

  [[nodiscard]] const T& value() const& {
    require_value();
    return std::get<kValueIndex>(storage_);
  }
  [[nodiscard]] T& value() & {
    require_value();
    return std::get<kValueIndex>(storage_);
  }
  [[nodiscard]] T&& value() && {
    require_value();
    return std::get<kValueIndex>(std::move(storage_));
  }

  [[nodiscard]] const T* operator->() const { return &value(); }
  [[nodiscard]] T* operator->() { return &value(); }
  [[nodiscard]] const T& operator*() const& { return value(); }
  [[nodiscard]] T& operator*() & { return value(); }
  [[nodiscard]] T&& operator*() && { return std::move(*this).value(); }

  [[nodiscard]] const Reason& reason() const noexcept {
    static const Reason kSuccess{ReasonCode::ok, std::string{}};
    if (has_value()) {
      return kSuccess;
    }
    return std::get<kReasonIndex>(storage_);
  }

  [[nodiscard]] ReasonCode code() const noexcept { return reason().code; }

  // Re-type a failure for a different Outcome<T> return position.
  template <class U>
  [[nodiscard]] Outcome<U> propagate() const {
    return Outcome<U>::fail(reason());
  }

  // Replace the value while preserving a failure.
  template <class U, class Fn>
  [[nodiscard]] Outcome<U> map(Fn&& fn) const {
    if (!has_value()) {
      return Outcome<U>::fail(reason());
    }
    return Outcome<U>::ok(fn(std::get<kValueIndex>(storage_)));
  }

 private:
  explicit Outcome(Reason reason) : storage_(std::in_place_index<kReasonIndex>, std::move(reason)) {}

  void require_value() const {
    if (!has_value()) {
      throw std::logic_error("fo::Outcome: value() called on a failure outcome: " +
                             std::string(to_string(reason().code)));
    }
  }

  static constexpr std::size_t kValueIndex = 1;
  static constexpr std::size_t kReasonIndex = 2;

  std::variant<std::monostate, T, Reason> storage_;
};

using Status = Outcome<Nothing>;

[[nodiscard]] inline Status success() noexcept { return Status::ok(Nothing{}); }

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_OUTCOME_HPP
