// Facility Observatory - lock order auditing.
//
// The runtime acquires its locks in strictly increasing rank order and never
// upgrades a shared lock to an exclusive one. This audit makes that discipline
// an executed, inspectable property rather than a comment: every acquisition
// pushes a rank onto a thread-local stack, and an acquisition that does not
// strictly increase the top of that stack is recorded as a violation.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_LOCK_ORDER_HPP
#define FACILITY_OBSERVATORY_LOCK_ORDER_HPP

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "facility_observatory/export.hpp"

namespace fo {

enum class LockRank : std::uint8_t {
  lifecycle = 1,       // Observatory lifecycle transitions
  ingest = 2,          // serialises every mutation
  journal_queue = 3,   // hand-off to the journal writer thread
  journal_writer = 4,  // the writer thread's own durable append
  event_dispatch = 5,  // user event handler invocation
  store = 6,           // the evidence store itself: always innermost
};

FO_API std::string_view to_string(LockRank rank) noexcept;

class FO_API LockOrderAudit {
 public:
  // Violations observed on the calling thread.
  [[nodiscard]] static std::uint64_t violations() noexcept;
  [[nodiscard]] static std::string last_violation();
  [[nodiscard]] static std::size_t depth() noexcept;
  static void reset() noexcept;
};

class FO_API LockOrderGuard {
 public:
  explicit LockOrderGuard(LockRank rank);
  ~LockOrderGuard();
  LockOrderGuard(const LockOrderGuard&) = delete;
  LockOrderGuard& operator=(const LockOrderGuard&) = delete;

  [[nodiscard]] bool violated() const noexcept { return violated_; }

 private:
  LockRank rank_{LockRank::lifecycle};
  bool violated_{false};
  bool pushed_{false};
};

}  // namespace fo

#endif  // FACILITY_OBSERVATORY_LOCK_ORDER_HPP
