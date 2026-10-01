// Facility Observatory - end-to-end CLI tests against the installed command.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdlib>
#include <string>
#include <vector>

#include "facility_observatory/json.hpp"
#include "facility_observatory/platform.hpp"
#include "harness.hpp"

using namespace fo;
using namespace fotest;

namespace {

std::string cli_path() {
  const std::vector<std::string>& args = arguments();
  return args.size() > 1 ? args[1] : std::string{};
}

std::string quote(const std::string& text) { return "\"" + text + "\""; }

struct CliResult {
  int status{0};
  std::string standard_output{};
  std::string standard_error{};
  Outcome<JsonValue> document{Outcome<JsonValue>::fail(ReasonCode::internal_error, "not parsed")};
};

CliResult run(const std::vector<std::string>& arguments_text, const std::string& workspace,
              const std::string& tag) {
  const std::string out_path = workspace + "/" + tag + ".out";
  const std::string err_path = workspace + "/" + tag + ".err";
  std::string command = quote(cli_path());
  for (const std::string& argument : arguments_text) {
    command.append(" ");
    command.append(quote(argument));
  }
  command.append(" > ");
  command.append(quote(out_path));
  command.append(" 2> ");
  command.append(quote(err_path));
  // cmd.exe strips the outermost quote pair when the line starts with a quote,
  // so the command is wrapped once more to protect the quoted executable path.
  const std::string wrapped = quote(command);

  CliResult result;
  const int status = std::system(wrapped.c_str());
#if defined(_WIN32)
  result.status = status;
#else
  result.status = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif

  auto out = read_whole_file(out_path, 16U << 20);
  if (out) {
    result.standard_output.assign(out.value().begin(), out.value().end());
  }
  auto err = read_whole_file(err_path, 16U << 20);
  if (err) {
    result.standard_error.assign(err.value().begin(), err.value().end());
  }
  if (!result.standard_output.empty()) {
    result.document = JsonValue::parse(result.standard_output);
  }
  return result;
}

const JsonValue* member(const JsonValue& document, const char* key) {
  return document.is_object() ? document.find(key) : nullptr;
}

std::int64_t integer_member(const JsonValue& document, const char* key) {
  const JsonValue* value = member(document, key);
  return value == nullptr ? -1 : value->integer_or(-1);
}

std::string string_member(const JsonValue& document, const char* key) {
  const JsonValue* value = member(document, key);
  return value == nullptr ? std::string{} : std::string(value->string_or(""));
}

std::string write_workspace(const std::string& directory) {
  const std::string path = directory + "/ingest.jsonl";
  const std::string lines =
      "{\"kind\":\"authority\",\"id\":\"dccp-a\",\"authority_kind\":\"dccp\",\"role\":\"authority\",\"label\":\"dc\","
      "\"epoch\":1}\n"
      "{\"kind\":\"authority\",\"id\":\"bms-a\",\"authority_kind\":\"bms\",\"role\":\"authority\",\"label\":\"bm\","
      "\"epoch\":1}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"topology\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r1\",\"aspect\":\"topology.parent\","
      "\"value\":{\"kind\":\"text\",\"text\":\"site:sea1\"}}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"topology\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r2\",\"aspect\":\"topology.parent\","
      "\"value\":{\"kind\":\"text\",\"text\":\"site:sea1\"}}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r1\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"1500\",\"unit\":\"watts\"}}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r2\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"1500\",\"unit\":\"watts\"}}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"site:sea1\",\"aspect\":\"power.total\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"3\",\"unit\":\"kilowatts\"}}\n"
      "{\"kind\":\"evidence\",\"authority\":\"bms-a\",\"authority_kind\":\"bms\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"rack:r1\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"1.6\",\"unit\":\"kilowatts\"}}\n";
  FO_REQUIRE(publish_atomically(path, lines.data(), lines.size()).has_value());
  return path;
}

}  // namespace

FO_TEST(cli_is_available_and_reports_its_identity) {
  FO_REQUIRE_MESSAGE(!cli_path().empty(), "the CLI path was not passed to this suite");
  TempDir workspace;
  const CliResult version = run({"version"}, workspace.path(), "version");
  FO_REQUIRE_EQ(version.status, 0);
  FO_REQUIRE(version.document.has_value());
  FO_REQUIRE_EQ(string_member(version.document.value(), "library_version"), std::string("1.0.0"));
  FO_REQUIRE_EQ(integer_member(version.document.value(), "journal_format_version"), 1);
}

FO_TEST(cli_self_test_passes_on_the_built_artifact) {
  TempDir workspace;
  const CliResult result = run({"selftest"}, workspace.path(), "selftest");
  FO_REQUIRE_EQ(result.status, 0);
  FO_REQUIRE(result.document.has_value());
  FO_REQUIRE_EQ(integer_member(result.document.value(), "failed"), 0);
  FO_REQUIRE(integer_member(result.document.value(), "passed") >= 15);
}

FO_TEST(cli_usage_and_unknown_commands_are_refused) {
  TempDir workspace;
  FO_REQUIRE_EQ(run({}, workspace.path(), "noargs").status, 2);
  FO_REQUIRE_EQ(run({"nonsense-command"}, workspace.path(), "unknown").status, 2);
  FO_REQUIRE_EQ(run({"help"}, workspace.path(), "help").status, 0);
  FO_REQUIRE_EQ(run({"view"}, workspace.path(), "noview").status, 2);
}

FO_TEST(cli_end_to_end_observation_flow) {
  TempDir workspace;
  const std::string journal = workspace.file("observatory.foj");
  const std::string ingest = write_workspace(workspace.path());

  const CliResult ingested = run({"ingest", "--journal", journal, "--file", ingest}, workspace.path(), "ingest");
  FO_REQUIRE_EQ(ingested.status, 0);
  FO_REQUIRE(ingested.document.has_value());
  FO_REQUIRE_EQ(integer_member(ingested.document.value(), "accepted"), 8);
  FO_REQUIRE_EQ(integer_member(ingested.document.value(), "refused"), 0);

  const CliResult verified = run({"verify", "--journal", journal}, workspace.path(), "verify");
  FO_REQUIRE_EQ(verified.status, 0);
  FO_REQUIRE(verified.document.has_value());
  FO_REQUIRE(verified.document.value().find("clean")->boolean_or(false));

  const CliResult status = run({"status", "--journal", journal}, workspace.path(), "status");
  FO_REQUIRE_EQ(status.status, 0);
  FO_REQUIRE_EQ(integer_member(status.document.value(), "records"), 6);
  FO_REQUIRE_EQ(integer_member(status.document.value(), "authorities"), 2);
  FO_REQUIRE_EQ(integer_member(status.document.value(), "subjects"), 3);

  const CliResult subjects = run({"subjects", "--journal", journal}, workspace.path(), "subjects");
  FO_REQUIRE_EQ(subjects.status, 0);
  FO_REQUIRE(subjects.document.value().find("subjects") != nullptr);

  // Two authorities disagree about rack:r1's draw, so the view is conflicting.
  const CliResult view = run({"view", "--journal", journal, "--subject", "rack:r1", "--aspect", "power.draw"},
                             workspace.path(), "view");
  FO_REQUIRE_EQ(view.status, 4);
  FO_REQUIRE_EQ(string_member(view.document.value(), "state"), std::string("conflicting"));
  const JsonValue* disagreement = view.document.value().find("disagreement");
  FO_REQUIRE(disagreement != nullptr);
  FO_REQUIRE_EQ(disagreement->size(), 2);

  const CliResult consistent = run({"view", "--journal", journal, "--subject", "rack:r2", "--aspect", "power.draw"},
                                   workspace.path(), "view-consistent");
  FO_REQUIRE_EQ(consistent.status, 0);
  FO_REQUIRE_EQ(string_member(consistent.document.value(), "state"), std::string("stale"));

  const CliResult unknown = run({"view", "--journal", journal, "--subject", "rack:missing", "--aspect", "power.draw"},
                                workspace.path(), "view-missing");
  FO_REQUIRE_EQ(unknown.status, 3);

  const CliResult history = run({"history", "--journal", journal, "--subject", "rack:r1"}, workspace.path(), "history");
  FO_REQUIRE_EQ(history.status, 0);
  FO_REQUIRE(integer_member(history.document.value(), "count") >= 2);

  const CliResult divergence = run({"divergence", "--journal", journal}, workspace.path(), "divergence");
  FO_REQUIRE_EQ(divergence.status, 4);
  FO_REQUIRE(integer_member(divergence.document.value(), "count") >= 1);

  const CliResult aggregate =
      run({"aggregate", "--journal", journal, "--scope", "site:sea1", "--aspect", "power.draw"}, workspace.path(),
          "aggregate");
  FO_REQUIRE(aggregate.status == 0 || aggregate.status == 3);
  FO_REQUIRE(aggregate.document.has_value());

  const CliResult snapshot = run({"snapshot", "--journal", journal}, workspace.path(), "snapshot");
  FO_REQUIRE_EQ(snapshot.status, 0);
  const std::string digest = string_member(snapshot.document.value(), "digest");
  FO_REQUIRE_EQ(digest.size(), 16);
  const std::int64_t sequence = integer_member(snapshot.document.value(), "sequence");
  FO_REQUIRE(sequence >= 1);

  const CliResult rebuilt =
      run({"reconstruct", "--journal", journal, "--sequence", std::to_string(sequence)}, workspace.path(), "rebuild");
  FO_REQUIRE_EQ(rebuilt.status, 0);
  FO_REQUIRE(rebuilt.document.value().find("matches_live")->boolean_or(false));
  FO_REQUIRE_EQ(string_member(rebuilt.document.value(), "digest"), digest);
}

FO_TEST(cli_rejects_malformed_ingest_lines) {
  TempDir workspace;
  const std::string journal = workspace.file("bad.foj");
  const std::string input = workspace.file("bad.jsonl");
  const std::string lines =
      "{\"kind\":\"authority\",\"id\":\"dccp-a\",\"authority_kind\":\"dccp\",\"role\":\"authority\",\"epoch\":1}\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\"}\n"
      "not json at all\n"
      "{\"kind\":\"evidence\",\"authority\":\"dccp-a\",\"authority_kind\":\"dccp\",\"source\":\"telemetry\","
      "\"epoch\":1,\"generation\":1,\"revision\":1,\"observed_at\":\"2026-01-01T00:00:00Z\","
      "\"published_at\":\"2026-01-01T00:00:00Z\",\"subject\":\"asset:a1\",\"aspect\":\"power.draw\","
      "\"value\":{\"kind\":\"scalar\",\"decimal\":\"1\",\"unit\":\"watts\"}}\n";
  FO_REQUIRE(publish_atomically(input, lines.data(), lines.size()).has_value());

  const CliResult result = run({"ingest", "--journal", journal, "--file", input}, workspace.path(), "bad");
  FO_REQUIRE(result.status != 0);
  FO_REQUIRE_EQ(integer_member(result.document.value(), "accepted"), 1);
  FO_REQUIRE_EQ(integer_member(result.document.value(), "refused"), 3);
}

FO_TEST(cli_verify_reports_persistence_faults) {
  TempDir workspace;
  const std::string journal = workspace.file("corrupt.foj");
  const std::string ingest = write_workspace(workspace.path());
  FO_REQUIRE_EQ(run({"ingest", "--journal", journal, "--file", ingest}, workspace.path(), "seed").status, 0);

  std::vector<std::uint8_t> bytes;
  {
    auto content = read_whole_file(journal, 16U << 20);
    FO_REQUIRE(content.has_value());
    bytes = content.value();
  }
  FO_REQUIRE(bytes.size() > 80);
  // Damage a byte inside the second frame, leaving later frames intact.
  bytes[40] = static_cast<std::uint8_t>(bytes[40] ^ 0xFFU);
  FO_REQUIRE(publish_atomically(journal, bytes.data(), bytes.size()).has_value());

  const CliResult verified = run({"verify", "--journal", journal}, workspace.path(), "verify-corrupt");
  FO_REQUIRE_EQ(verified.status, 6);
  FO_REQUIRE(verified.document.value().find("interior_corruption")->boolean_or(false));

  const CliResult status = run({"status", "--journal", journal}, workspace.path(), "status-corrupt");
  FO_REQUIRE(status.status != 0);
}
