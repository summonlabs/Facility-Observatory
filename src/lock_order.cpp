// Facility Observatory - lock order auditing.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "facility_observatory/lock_order.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace fo {
namespace {

constexpr std::size_t kMaxDepth = 16;

struct AuditState {
  std::array<LockRank, kMaxDepth> stack{};
  std::size_t depth{0};
  std::uint64_t violations{0};
  std::string last{};
};

thread_local AuditState g_audit;

}  // namespace

std::string_view to_string(LockRank rank) noexcept {
  switch (rank) {
    case LockRank::lifecycle:
      return std::string_view{"lifecycle"};
    case LockRank::ingest:
      return std::string_view{"ingest"};
    case LockRank::journal_queue:
      return std::string_view{"journal_queue"};
    case LockRank::journal_writer:
      return std::string_view{"journal_writer"};
    case LockRank::event_dispatch:
      return std::string_view{"event_dispatch"};
    case LockRank::store:
      return std::string_view{"store"};
  }
  return std::string_view{"unknown"};
}

std::uint64_t LockOrderAudit::violations() noexcept { return g_audit.violations; }

std::string LockOrderAudit::last_violation() { return g_audit.last; }

std::size_t LockOrderAudit::depth() noexcept { return g_audit.depth; }

void LockOrderAudit::reset() noexcept {
  g_audit.depth = 0;
  g_audit.violations = 0;
  g_audit.last.clear();
}

LockOrderGuard::LockOrderGuard(LockRank rank) : rank_(rank) {
  std::string reason;
  if (g_audit.depth >= kMaxDepth) {
    reason = "lock nesting exceeded the audited depth of ";
    reason.append(std::to_string(kMaxDepth));
  } else if (g_audit.depth > 0 && rank <= g_audit.stack[g_audit.depth - 1]) {
    reason = "acquired '";
    reason.append(to_string(rank));
    reason.append("' while already holding '");
    reason.append(to_string(g_audit.stack[g_audit.depth - 1]));
    reason.append("'");
  }
  if (!reason.empty()) {
    violated_ = true;
    g_audit.violations += 1;
    g_audit.last = std::move(reason);
    return;
  }
  g_audit.stack[g_audit.depth] = rank;
  g_audit.depth += 1;
  pushed_ = true;
}

LockOrderGuard::~LockOrderGuard() {
  if (pushed_ && g_audit.depth > 0) {
    g_audit.depth -= 1;
  }
}

}  // namespace fo
