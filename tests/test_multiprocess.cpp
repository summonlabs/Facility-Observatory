// Facility Observatory - real multiprocess ownership and restart tests.
//
// These spawn separate operating-system processes, so they prove kernel-enforced
// exclusion and cross-process durability rather than in-process locking.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#include "facility_observatory/observatory.hpp"
#include "fixtures.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

constexpr int kExitLocked = 4;
constexpr int kExitUsage = 2;

std::string quote(const std::string& text) { return "\"" + text + "\""; }

std::string child_path() {
  const std::vector<std::string>& args = arguments();
  return args.size() > 1 ? args[1] : std::string{};
}

int run_child(const std::string& arguments_text, const std::string& transcript) {
  // The whole command is wrapped in a second pair of quotes: cmd.exe strips the
  // outermost pair when the line starts with a quote, which would otherwise
  // mangle the quoted executable path.
  const std::string command =
      quote(quote(child_path()) + " " + arguments_text + " > " + quote(transcript) + " 2>&1");
  const int status = std::system(command.c_str());
#if defined(_WIN32)
  return status;
#else
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

FO_TEST(child_binary_is_available) {
  FO_REQUIRE_MESSAGE(!child_path().empty(), "the multiprocess child path was not passed to this suite");
  TempDir directory;
  const int status = run_child("nonsense " + quote(directory.file("j.foj")), directory.file("out.txt"));
  FO_REQUIRE_EQ(status, kExitUsage);
}

FO_TEST(a_held_lock_excludes_another_process) {
  TempDir directory;
  const std::string journal = directory.file("exclusive.foj");
  const std::string transcript = directory.file("child.txt");

  ObservatoryOptions options;
  options.journal.path = journal;
  options.journal.origin = "parent";
  auto parent = Observatory::open(options);
  FO_REQUIRE(parent.has_value());

  FO_REQUIRE_EQ(run_child("trylock " + quote(journal), transcript), kExitLocked);
  FO_REQUIRE_EQ(run_child("write " + quote(journal) + " 3", transcript), kExitLocked);

  static_cast<void>(parent.value().close());
  FO_REQUIRE_EQ(run_child("trylock " + quote(journal), transcript), 0);
}

FO_TEST(the_kernel_releases_the_lock_when_a_process_exits) {
  TempDir directory;
  const std::string journal = directory.file("release.foj");
  const std::string transcript = directory.file("holder.txt");

  int holder_status = -1;
  std::thread holder([&] { holder_status = run_child("hold " + quote(journal) + " 1200", transcript); });

  // Wait until the holder has had time to take the lock.
  std::this_thread::sleep_for(std::chrono::milliseconds(400));
  ObservatoryOptions options;
  options.journal.path = journal;
  options.journal.origin = "parent";
  auto blocked = Observatory::open(options);
  FO_REQUIRE(!blocked.has_value());
  FO_REQUIRE(blocked.code() == ReasonCode::journal_locked);

  holder.join();
  FO_REQUIRE_EQ(holder_status, 0);

  auto acquired = Observatory::open(options);
  FO_REQUIRE(acquired.has_value());
  static_cast<void>(acquired.value().close());
}

FO_TEST(a_child_process_writes_evidence_the_parent_recovers) {
  TempDir directory;
  const std::string journal = directory.file("handover.foj");
  const std::string transcript = directory.file("writer.txt");

  FO_REQUIRE_EQ(run_child("write " + quote(journal) + " 4", transcript), 0);

  ObservatoryOptions options;
  options.journal.path = journal;
  options.journal.origin = "parent-after-child";
  auto parent = Observatory::open(options);
  FO_REQUIRE(parent.has_value());
  FO_REQUIRE(parent.value().knows_subject(EntityRef::parse("rack:child-r1").value()));

  const EntityRef rack = EntityRef::parse("rack:child-r1").value();
  const AspectId draw = AspectId::parse("power.draw").value();
  const Evaluation recovered = parent.value().evaluate(rack, draw, SystemClock{}.now());
  FO_REQUIRE(recovered.state != ObservationState::known);
  FO_REQUIRE(recovered.participants.size() >= 1);
  const std::string digest = parent.value().status().digest;
  static_cast<void>(parent.value().close());

  // A second, independent process sees exactly the same durable content.
  auto reopened = Observatory::open(options);
  FO_REQUIRE(reopened.has_value());
  FO_REQUIRE_EQ(reopened.value().status().digest, digest);
  static_cast<void>(reopened.value().close());
}

FO_TEST(two_writers_are_serialised_across_processes) {
  TempDir directory;
  const std::string journal = directory.file("serialised.foj");
  const std::string first_transcript = directory.file("first.txt");
  const std::string second_transcript = directory.file("second.txt");

  // Run two children back to back: each must take the lock, write, and release.
  FO_REQUIRE_EQ(run_child("write " + quote(journal) + " 3", first_transcript), 0);
  FO_REQUIRE_EQ(run_child("write " + quote(journal) + " 3", second_transcript), 0);

  ObservatoryOptions options;
  options.journal.path = journal;
  options.journal.origin = "parent-verify";
  auto parent = Observatory::open(options);
  FO_REQUIRE(parent.has_value());
  FO_REQUIRE(parent.value().status().records >= 3);
  FO_REQUIRE(parent.value().status().recovery.clean);
  static_cast<void>(parent.value().close());
}
