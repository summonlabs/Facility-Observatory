// Facility Observatory - command line interface.
//
// Every command produces one deterministic JSON document on stdout and one of a
// documented set of exit codes. Usage and failure details go to stderr.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "facility_observatory/contracts.hpp"
#include "facility_observatory/lock_order.hpp"
#include "facility_observatory/observatory.hpp"
#include "facility_observatory/render.hpp"
#include "facility_observatory/version.hpp"

namespace {

using namespace fo;

// Exit code contract, stable across releases.
enum class ExitCode : int {
  ok = 0,
  failure = 1,
  usage = 2,
  unresolved = 3,
  conflict = 4,
  refused = 5,
  persistence = 6,
};

struct Options {
  std::map<std::string, std::vector<std::string>> values;
  std::vector<std::string> positional;
  bool pretty{false};

  [[nodiscard]] bool has(const std::string& key) const { return values.find(key) != values.end(); }

  [[nodiscard]] std::optional<std::string> single(const std::string& key) const {
    const auto found = values.find(key);
    if (found == values.end() || found->second.empty()) {
      return std::nullopt;
    }
    return found->second.back();
  }

  [[nodiscard]] std::vector<std::string> many(const std::string& key) const {
    const auto found = values.find(key);
    if (found == values.end()) {
      return {};
    }
    return found->second;
  }
};

ExitCode exit_for(ReasonCode code) {
  switch (code) {
    case ReasonCode::ok:
      return ExitCode::ok;
    case ReasonCode::empty_input:
    case ReasonCode::invalid_argument:
    case ReasonCode::invalid_identifier:
    case ReasonCode::identifier_too_long:
    case ReasonCode::invalid_aspect_path:
    case ReasonCode::malformed_encoding:
    case ReasonCode::scale_mismatch:
    case ReasonCode::inexact_conversion:
    case ReasonCode::value_out_of_range:
    case ReasonCode::unit_incompatible:
    case ReasonCode::arithmetic_overflow:
    case ReasonCode::division_by_zero:
    case ReasonCode::record_too_large:
    case ReasonCode::too_many_records:
      return ExitCode::usage;
    case ReasonCode::mutation_refused:
    case ReasonCode::authority_boundary_violation:
    case ReasonCode::authority_role_not_observer:
      return ExitCode::refused;
    case ReasonCode::conflicting_evidence:
    case ReasonCode::indeterminate:
      return ExitCode::conflict;
    case ReasonCode::no_evidence:
    case ReasonCode::evidence_not_found:
    case ReasonCode::unknown_entity:
    case ReasonCode::unknown_domain:
    case ReasonCode::unsupported_query:
    case ReasonCode::not_supported:
    case ReasonCode::unsupported_aspect:
    case ReasonCode::authority_unknown:
    case ReasonCode::source_not_registered:
      return ExitCode::unresolved;
    default:
      break;
  }
  if (code >= ReasonCode::journal_missing && code <= ReasonCode::lock_unavailable) {
    return ExitCode::persistence;
  }
  return ExitCode::failure;
}

ExitCode exit_for_state(ObservationState state) {
  switch (state) {
    case ObservationState::known:
    case ObservationState::stale:
      return ExitCode::ok;
    case ObservationState::conflicting:
      return ExitCode::conflict;
    case ObservationState::unknown:
    case ObservationState::unsupported:
      return ExitCode::unresolved;
    case ObservationState::indeterminate:
      return ExitCode::conflict;
  }
  return ExitCode::failure;
}

void usage() {
  std::fprintf(stderr,
               "facility-observatory %s - read-only DCCP evidence observatory\n"
               "\n"
               "usage: facility-observatory <command> [options]\n"
               "\n"
               "commands:\n"
               "  version                                     print the runtime identity\n"
               "  selftest                                    run built-in deterministic checks\n"
               "  verify      --journal <path>                scan the journal without modifying it\n"
               "  ingest      --journal <path> [--file <p>]   record JSON-lines observations (stdin if no --file)\n"
               "  status      --journal <path>                summary of what is currently held\n"
               "  subjects    --journal <path> [--domain <d>] list known subjects\n"
               "  view        --journal <path> --subject <s> [--aspect <a>] [--at <ts>]\n"
               "  aggregate   --journal <path> --aspect <a> [--subject <s>]... [--kind <k>] [--allow-partial]\n"
               "  divergence  --journal <path> [--subject <s>] [--at <ts>]\n"
               "  history     --journal <path> --subject <s> [--aspect <a>]\n"
               "  snapshot    --journal <path> [--at <ts>]\n"
               "  reconstruct --journal <path> --sequence <n> [--at <ts>]\n"
               "\n"
               "global options:\n"
               "  --pretty          indent the JSON output\n"
               "  --at <rfc3339>    evaluation instant; defaults to the newest durable publication\n"
               "  --read-only       never take the writer lock and never modify the journal\n"
               "                    (implied by verify; available for the query commands)\n"
               "\n"
               "exit codes:\n"
               "  0 known or produced     1 internal failure      2 usage or malformed input\n"
               "  3 unknown or unsupported 4 conflicting          5 boundary refusal\n"
               "  6 persistence fault\n",
               std::string(version_string()).c_str());
}

void emit(const JsonValue& document, bool pretty, int indent = 2) {
  std::string text = document.dump(pretty ? indent : 0);
  text.push_back('\n');
  std::fwrite(text.data(), 1, text.size(), stdout);
}

ExitCode fail(ReasonCode code, const std::string& detail, bool pretty) {
  JsonValue document = JsonValue::make_object();
  document.set("code", JsonValue::make_string(std::string(to_string(code))));
  document.set("detail", JsonValue::make_string(detail));
  emit(document, pretty);
  std::fprintf(stderr, "facility-observatory: %s: %s\n", std::string(to_string(code)).c_str(), detail.c_str());
  return exit_for(code);
}

std::optional<Timestamp> parse_at(const Options& options, std::string& error) {
  auto text = options.single("--at");
  if (!text.has_value()) {
    return std::nullopt;
  }
  auto parsed = Timestamp::parse_rfc3339(text.value());
  if (!parsed) {
    error = parsed.reason().detail;
    return std::nullopt;
  }
  return parsed.value();
}

Timestamp newest_instant(const Observatory& observatory) {
  Timestamp newest;
  for (const EntityRef& entity : observatory.subjects()) {
    auto records = observatory.history(entity);
    if (!records) {
      continue;
    }
    for (const EvidenceRecord& record : records.value()) {
      if (record.published_at.is_set() && (!newest.is_set() || record.published_at > newest)) {
        newest = record.published_at;
      }
    }
  }
  return newest;
}

std::optional<EntityRef> parse_subject(const std::string& text, std::string& error) {
  auto parsed = EntityRef::parse(text);
  if (!parsed) {
    error = parsed.reason().detail;
    return std::nullopt;
  }
  return parsed.value();
}

std::optional<AspectId> parse_aspect(const std::string& text, std::string& error) {
  auto parsed = AspectId::parse(text);
  if (!parsed) {
    error = parsed.reason().detail;
    return std::nullopt;
  }
  return parsed.value();
}

ObservatoryOptions open_options(const Options& options, bool read_only) {
  ObservatoryOptions settings;
  settings.journal.path = options.single("--journal").value_or("");
  settings.journal.read_only = read_only;
  settings.journal.origin = "cli";
  return settings;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------
ExitCode command_version(bool pretty) { emit(render_version(), pretty); return ExitCode::ok; }

ExitCode command_verify(const Options& options, bool pretty) {
  const std::string path = options.single("--journal").value_or("");
  if (path.empty()) {
    return fail(ReasonCode::journal_path_invalid, "--journal is required", pretty);
  }
  auto report = inspect_journal(path);
  if (!report) {
    return fail(report.code(), report.reason().detail, pretty);
  }
  emit(render_recovery(report.value()), pretty);
  if (report.value().interior_corruption) {
    return ExitCode::persistence;
  }
  if (report.value().torn_tail) {
    return ExitCode::persistence;
  }
  return ExitCode::ok;
}

ExitCode command_status(const Options& options, bool pretty) {
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  emit(render_status(observatory.value().status()), pretty);
  return exit_for(observatory.value().status().recovery.code);
}

ExitCode command_subjects(const Options& options, bool pretty) {
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  std::optional<Domain> domain;
  if (auto text = options.single("--domain"); text.has_value()) {
    auto parsed = domain_from_string(text.value());
    if (!parsed) {
      return fail(parsed.code(), parsed.reason().detail, pretty);
    }
    domain = parsed.value();
  }
  const std::vector<EntityRef> subjects =
      domain.has_value() ? observatory.value().subjects(domain.value()) : observatory.value().subjects();
  JsonValue array = JsonValue::make_array();
  for (const EntityRef& entity : subjects) {
    JsonValue entry = JsonValue::make_object();
    entry.set("subject", JsonValue::make_string(entity.to_string()));
    JsonValue aspects = JsonValue::make_array();
    for (const AspectId& aspect : observatory.value().aspects_of(entity)) {
      aspects.push_back(JsonValue::make_string(aspect.str()));
    }
    entry.set("aspects", std::move(aspects));
    array.push_back(std::move(entry));
  }
  JsonValue document = JsonValue::make_object();
  document.set("subjects", std::move(array));
  emit(document, pretty);
  return ExitCode::ok;
}

ExitCode command_view(const Options& options, bool pretty) {
  std::string error;
  const auto subject_text = options.single("--subject");
  if (!subject_text.has_value()) {
    return fail(ReasonCode::invalid_argument, "--subject is required", pretty);
  }
  auto subject = parse_subject(subject_text.value(), error);
  if (!subject.has_value()) {
    return fail(ReasonCode::invalid_identifier, error, pretty);
  }
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  auto at = parse_at(options, error);
  if (!error.empty()) {
    return fail(ReasonCode::invalid_argument, error, pretty);
  }
  const Timestamp instant = at.has_value() ? at.value() : newest_instant(observatory.value());

  auto aspect_text = options.single("--aspect");
  if (aspect_text.has_value()) {
    auto aspect = parse_aspect(aspect_text.value(), error);
    if (!aspect.has_value()) {
      return fail(ReasonCode::invalid_aspect_path, error, pretty);
    }
    const Evaluation evaluation = observatory.value().evaluate(subject.value(), aspect.value(), instant);
    emit(render_evaluation(evaluation), pretty);
    return exit_for_state(evaluation.state);
  }

  JsonValue array = JsonValue::make_array();
  ObservationState worst = ObservationState::known;
  for (const AspectId& aspect : observatory.value().aspects_of(subject.value())) {
    const Evaluation evaluation = observatory.value().evaluate(subject.value(), aspect, instant);
    if (evaluation.state != ObservationState::known && worst == ObservationState::known) {
      worst = evaluation.state;
    }
    array.push_back(render_evaluation(evaluation));
  }
  if (array.size() == 0) {
    return fail(ReasonCode::unknown_entity,
                "no authority has published any aspect for " + subject.value().to_string(), pretty);
  }
  JsonValue document = JsonValue::make_object();
  document.set("subject", JsonValue::make_string(subject.value().to_string()));
  document.set("at", JsonValue::make_string(instant.is_set() ? instant.to_rfc3339() : std::string("unset")));
  document.set("evaluations", std::move(array));
  emit(document, pretty);
  return array.size() == 0 ? ExitCode::unresolved : exit_for_state(worst);
}

ExitCode command_aggregate(const Options& options, bool pretty) {
  std::string error;
  auto aspect_text = options.single("--aspect");
  if (!aspect_text.has_value()) {
    return fail(ReasonCode::invalid_argument, "--aspect is required", pretty);
  }
  auto aspect = parse_aspect(aspect_text.value(), error);
  if (!aspect.has_value()) {
    return fail(ReasonCode::invalid_aspect_path, error, pretty);
  }
  Aggregation kind = Aggregation::sum;
  if (auto text = options.single("--kind"); text.has_value()) {
    auto parsed = aggregation_from_string(text.value());
    if (!parsed) {
      return fail(parsed.code(), parsed.reason().detail, pretty);
    }
    kind = parsed.value();
  }
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  auto at = parse_at(options, error);
  if (!error.empty()) {
    return fail(ReasonCode::invalid_argument, error, pretty);
  }
  const Timestamp instant = at.has_value() ? at.value() : newest_instant(observatory.value());

  std::vector<EntityRef> subjects;
  EntityRef scope;
  for (const std::string& text : options.many("--subject")) {
    auto parsed = parse_subject(text, error);
    if (!parsed.has_value()) {
      return fail(ReasonCode::invalid_identifier, error, pretty);
    }
    subjects.push_back(parsed.value());
  }
  if (subjects.empty()) {
    if (auto scope_text = options.single("--scope"); scope_text.has_value()) {
      auto parsed = parse_subject(scope_text.value(), error);
      if (!parsed.has_value()) {
        return fail(ReasonCode::invalid_identifier, error, pretty);
      }
      scope = parsed.value();
      // Constituents are those subjects whose containment evidence names the scope.
      auto conventions = TopologyConventions::make("topology.parent", "power.total", "power.draw");
      if (!conventions) {
        return fail(conventions.code(), conventions.reason().detail, pretty);
      }
      const std::string wanted = scope.to_string();
      for (const EntityRef& entity : observatory.value().subjects()) {
        if (entity == scope) {
          continue;
        }
        const Evaluation containment =
            observatory.value().evaluate(entity, conventions.value().containment_aspect, instant);
        if (containment.value.has_value() && containment.value.value().kind() == ValueKind::text &&
            containment.value.value().text_body() == wanted) {
          subjects.push_back(entity);
        }
      }
    }
  }
  if (subjects.empty()) {
    return fail(ReasonCode::invalid_argument, "at least one --subject or a --scope is required", pretty);
  }

  const AggregateResult result = observatory.value().aggregate(scope, subjects, aspect.value(), kind,
                                                              !options.has("--allow-partial"), instant);
  emit(render_aggregate(result), pretty);
  return exit_for_state(result.state);
}

ExitCode command_divergence(const Options& options, bool pretty) {
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  std::string error;
  auto at = parse_at(options, error);
  if (!error.empty()) {
    return fail(ReasonCode::invalid_argument, error, pretty);
  }
  const Timestamp instant = at.has_value() ? at.value() : newest_instant(observatory.value());

  Outcome<std::vector<Divergence>> found = Outcome<std::vector<Divergence>>::ok(std::vector<Divergence>{});
  if (auto subject_text = options.single("--subject"); subject_text.has_value()) {
    auto subject = parse_subject(subject_text.value(), error);
    if (!subject.has_value()) {
      return fail(ReasonCode::invalid_identifier, error, pretty);
    }
    found = observatory.value().divergences_for(subject.value(), instant);
  } else {
    found = observatory.value().divergences(instant);
  }
  if (!found) {
    return fail(found.code(), found.reason().detail, pretty);
  }
  JsonValue array = JsonValue::make_array();
  for (const Divergence& divergence : found.value()) {
    array.push_back(render_divergence(divergence));
  }
  JsonValue document = JsonValue::make_object();
  document.set("at", JsonValue::make_string(instant.is_set() ? instant.to_rfc3339() : std::string("unset")));
  document.set("count", JsonValue::make_integer(static_cast<std::int64_t>(found.value().size())));
  document.set("divergences", std::move(array));
  emit(document, pretty);
  return found.value().empty() ? ExitCode::ok : ExitCode::conflict;
}

ExitCode command_history(const Options& options, bool pretty) {
  std::string error;
  auto subject_text = options.single("--subject");
  if (!subject_text.has_value()) {
    return fail(ReasonCode::invalid_argument, "--subject is required", pretty);
  }
  auto subject = parse_subject(subject_text.value(), error);
  if (!subject.has_value()) {
    return fail(ReasonCode::invalid_identifier, error, pretty);
  }
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }

  std::vector<EvidenceRecord> records;
  if (auto aspect_text = options.single("--aspect"); aspect_text.has_value()) {
    auto aspect = parse_aspect(aspect_text.value(), error);
    if (!aspect.has_value()) {
      return fail(ReasonCode::invalid_aspect_path, error, pretty);
    }
    auto history = observatory.value().history(subject.value(), aspect.value());
    if (!history) {
      return fail(history.code(), history.reason().detail, pretty);
    }
    records = history.value();
  } else {
    auto history = observatory.value().history(subject.value());
    if (!history) {
      return fail(history.code(), history.reason().detail, pretty);
    }
    records = history.value();
  }
  if (records.empty()) {
    return fail(ReasonCode::evidence_not_found,
                "no history is held for " + subject.value().to_string(), pretty);
  }
  JsonValue array = JsonValue::make_array();
  for (const EvidenceRecord& record : records) {
    array.push_back(render_evidence_record(record));
  }
  JsonValue document = JsonValue::make_object();
  document.set("subject", JsonValue::make_string(subject.value().to_string()));
  document.set("count", JsonValue::make_integer(static_cast<std::int64_t>(records.size())));
  document.set("records", std::move(array));
  emit(document, pretty);
  return ExitCode::ok;
}

ExitCode command_snapshot(const Options& options, bool pretty) {
  auto observatory = Observatory::open(open_options(options, false));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  std::string error;
  auto at = parse_at(options, error);
  if (!error.empty()) {
    return fail(ReasonCode::invalid_argument, error, pretty);
  }
  auto snapshot = observatory.value().snapshot(at.has_value() ? at.value() : Timestamp{});
  if (!snapshot) {
    return fail(snapshot.code(), snapshot.reason().detail, pretty);
  }
  auto closed = observatory.value().close();
  static_cast<void>(closed);
  emit(render_snapshot(snapshot.value()), pretty);
  return ExitCode::ok;
}

ExitCode command_reconstruct(const Options& options, bool pretty) {
  auto sequence_text = options.single("--sequence");
  if (!sequence_text.has_value()) {
    return fail(ReasonCode::invalid_argument, "--sequence is required", pretty);
  }
  std::uint64_t sequence = 0;
  try {
    sequence = static_cast<std::uint64_t>(std::stoull(sequence_text.value()));
  } catch (const std::exception&) {
    return fail(ReasonCode::invalid_argument, "--sequence must be a non-negative integer", pretty);
  }
  auto observatory = Observatory::open(open_options(options, true));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }
  std::string error;
  auto at = parse_at(options, error);
  if (!error.empty()) {
    return fail(ReasonCode::invalid_argument, error, pretty);
  }
  auto rebuilt = observatory.value().reconstruct(JournalSequence::from_value(sequence),
                                                 at.has_value() ? at.value() : Timestamp{});
  if (!rebuilt) {
    return fail(rebuilt.code(), rebuilt.reason().detail, pretty);
  }
  const std::string live_digest = observatory.value().status().digest;
  JsonValue document = render_snapshot(rebuilt.value());
  document.set("live_digest", JsonValue::make_string(live_digest));
  document.set("matches_live", JsonValue::make_boolean(live_digest == rebuilt.value().digest));
  emit(document, pretty);
  return ExitCode::ok;
}

ExitCode command_ingest(const Options& options, bool pretty) {
  const std::string path = options.single("--journal").value_or("");
  if (path.empty()) {
    return fail(ReasonCode::journal_path_invalid, "--journal is required", pretty);
  }
  auto observatory = Observatory::open(open_options(options, false));
  if (!observatory) {
    return fail(observatory.code(), observatory.reason().detail, pretty);
  }

  std::vector<std::string> lines;
  if (auto file = options.single("--file"); file.has_value()) {
    auto content = read_whole_file(file.value(), 64U << 20);
    if (!content) {
      return fail(content.code(), content.reason().detail, pretty);
    }
    std::string current;
    for (const std::uint8_t byte : content.value()) {
      if (byte == '\n') {
        lines.push_back(current);
        current.clear();
        continue;
      }
      if (byte != '\r') {
        current.push_back(static_cast<char>(byte));
      }
    }
    if (!current.empty()) {
      lines.push_back(current);
    }
  } else {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }
      lines.push_back(line);
    }
  }

  JsonValue results = JsonValue::make_array();
  std::size_t accepted = 0;
  std::size_t refused = 0;
  ExitCode worst = ExitCode::ok;
  std::size_t line_number = 0;
  for (const std::string& line : lines) {
    ++line_number;
    if (line.empty()) {
      continue;
    }
    JsonValue entry = JsonValue::make_object();
    entry.set("line", JsonValue::make_integer(static_cast<std::int64_t>(line_number)));
    auto document = JsonValue::parse(line);
    if (!document) {
      entry.set("code", JsonValue::make_string(std::string(to_string(document.code()))));
      entry.set("detail", JsonValue::make_string(document.reason().detail));
      results.push_back(std::move(entry));
      refused += 1;
      if (worst == ExitCode::ok) {
        worst = exit_for(document.code());
      }
      continue;
    }
    auto ingest = ingest_document_from_json(document.value());
    if (!ingest) {
      entry.set("code", JsonValue::make_string(std::string(to_string(ingest.code()))));
      entry.set("detail", JsonValue::make_string(ingest.reason().detail));
      results.push_back(std::move(entry));
      refused += 1;
      if (worst == ExitCode::ok) {
        worst = exit_for(ingest.code());
      }
      continue;
    }

    const IngestDocument& parsed = ingest.value();
    if (parsed.kind == IngestDocument::Kind::authority) {
      auto registered = observatory.value().register_authority(parsed.authority);
      if (!registered) {
        entry.set("code", JsonValue::make_string(std::string(to_string(registered.code()))));
        entry.set("detail", JsonValue::make_string(registered.reason().detail));
        refused += 1;
        if (worst == ExitCode::ok) {
          worst = exit_for(registered.code());
        }
      } else {
        entry.set("code", JsonValue::make_string("ok"));
        entry.set("detail", JsonValue::make_string("authority registered"));
        entry.set("authority", render_authority(registered.value()));
        accepted += 1;
      }
      results.push_back(std::move(entry));
      continue;
    }
    if (parsed.kind == IngestDocument::Kind::epoch) {
      const IngestionOutcome outcome =
          observatory.value().advance_epoch(parsed.epoch_authority, parsed.epoch, parsed.published_at);
      results.push_back(render_ingestion(outcome));
      if (outcome.code == ReasonCode::ok) {
        accepted += 1;
      } else {
        refused += 1;
        if (worst == ExitCode::ok) {
          worst = exit_for(outcome.code);
        }
      }
      continue;
    }

    auto record = to_evidence(parsed.envelope, parsed.envelope.published_at);
    if (!record) {
      entry.set("code", JsonValue::make_string(std::string(to_string(record.code()))));
      entry.set("detail", JsonValue::make_string(record.reason().detail));
      results.push_back(std::move(entry));
      refused += 1;
      if (worst == ExitCode::ok) {
        worst = exit_for(record.code());
      }
      continue;
    }
    const IngestionOutcome outcome = observatory.value().record_evidence(record.value());
    results.push_back(render_ingestion(outcome));
    const bool benign = outcome.code == ReasonCode::ok || outcome.code == ReasonCode::duplicate_evidence ||
                        outcome.code == ReasonCode::superseded_revision ||
                        outcome.code == ReasonCode::stale_epoch ||
                        outcome.code == ReasonCode::conflicting_evidence;
    if (benign) {
      accepted += 1;
    } else {
      refused += 1;
      if (worst == ExitCode::ok) {
        worst = exit_for(outcome.code);
      }
    }
  }

  auto closed = observatory.value().close();
  static_cast<void>(closed);

  JsonValue summary = JsonValue::make_object();
  summary.set("accepted", JsonValue::make_integer(static_cast<std::int64_t>(accepted)));
  summary.set("refused", JsonValue::make_integer(static_cast<std::int64_t>(refused)));
  summary.set("lines", JsonValue::make_integer(static_cast<std::int64_t>(lines.size())));
  summary.set("results", std::move(results));
  emit(summary, pretty);
  return refused == 0 ? ExitCode::ok : worst;
}

// ---------------------------------------------------------------------------
// Built-in self test
//
// Exercises the load-bearing invariants without the test suite, so an installed
// artifact can be validated where the build tree is not present.
// ---------------------------------------------------------------------------
struct SelfTest {
  std::size_t passed{0};
  std::size_t failed{0};
  JsonValue checks{JsonValue::make_array()};

  void record(std::string_view name, bool ok, std::string detail = std::string{}) {
    JsonValue entry = JsonValue::make_object();
    entry.set("check", JsonValue::make_string(std::string(name)));
    entry.set("ok", JsonValue::make_boolean(ok));
    if (!detail.empty()) {
      entry.set("detail", JsonValue::make_string(std::move(detail)));
    }
    checks.push_back(std::move(entry));
    if (ok) {
      passed += 1;
    } else {
      failed += 1;
    }
  }
};

Timestamp at_seconds(std::int64_t seconds) {
  return Timestamp::from_unix_nanos(seconds * 1000000000LL).value();
}

ExitCode command_selftest(bool pretty) {
  SelfTest test;

  auto sum = FixedPoint::parse("1.5");
  auto other = FixedPoint::parse("2.25");
  test.record("fixed-point exact addition", sum && other && sum.value().add(other.value()) &&
                                                sum.value().add(other.value()).value().to_string() == "3.75");

  auto overflow = FixedPoint::from_scaled(std::numeric_limits<std::int64_t>::max(), 0);
  test.record("checked arithmetic refuses overflow",
              overflow && !overflow.value().add(FixedPoint::from_scaled(1, 0).value()).has_value());

  auto kilowatts = FixedPoint::parse("1.5");
  auto watts = kilowatts ? convert(kilowatts.value(), Unit::kilowatts, Unit::watts) : Outcome<FixedPoint>::fail(ReasonCode::internal_error);
  test.record("unit conversion is exact", watts && watts.value().to_string() == "1500.0",
              watts ? watts.value().to_string() : std::string("conversion failed"));

  auto celsius = FixedPoint::parse("25");
  auto kelvin = celsius ? convert(celsius.value(), Unit::celsius, Unit::kelvin) : Outcome<FixedPoint>::fail(ReasonCode::internal_error);
  test.record("affine temperature conversion", kelvin && kelvin.value().to_string() == "298.15",
              kelvin ? kelvin.value().to_string() : std::string("conversion failed"));

  auto stamp = Timestamp::parse_rfc3339("2026-03-01T12:34:56.789Z");
  test.record("rfc3339 round trip", stamp && stamp.value().to_rfc3339() == "2026-03-01T12:34:56.789Z",
              stamp ? stamp.value().to_rfc3339() : std::string("parse failed"));

  test.record("rfc3339 rejects leap seconds", !Timestamp::parse_rfc3339("2026-03-01T12:34:60Z").has_value());
  test.record("utf8 validation rejects overlong forms", !is_valid_utf8("\xC0\xAF"));

  const std::string root =
      (std::filesystem::temp_directory_path() / ("fo-selftest-" + unique_suffix())).string();
  std::error_code error;
  std::filesystem::create_directories(root, error);
  const std::string journal_path = root + "/journal.foj";

  {
    ObservatoryOptions settings;
    settings.journal.path = journal_path;
    settings.journal.origin = "selftest";
    auto observatory = Observatory::open(settings);
    test.record("journal opens and creates", observatory.has_value(),
                observatory ? std::string{} : observatory.reason().detail);
    if (observatory) {
      AuthorityDescriptor dccp;
      dccp.id = AuthorityId::parse("dccp-a").value();
      dccp.kind = AuthorityKind::dccp;
      dccp.role = AuthorityRole::authority;
      dccp.epoch = Epoch::from_value(1);
      auto registered = observatory.value().register_authority(dccp);
      test.record("authority registration is durable", registered.has_value());

      AuthorityDescriptor bms;
      bms.id = AuthorityId::parse("bms-a").value();
      bms.kind = AuthorityKind::bms;
      bms.role = AuthorityRole::authority;
      bms.epoch = Epoch::from_value(1);
      static_cast<void>(observatory.value().register_authority(bms));

      const EntityRef rack = EntityRef::parse("rack:sea1-r07").value();
      const AspectId draw = AspectId::parse("power.draw").value();

      EvidenceRecord first;
      first.key.authority = dccp.id;
      first.key.source = SourceId::parse("telemetry").value();
      first.key.entity = rack;
      first.key.aspect = draw;
      first.epoch = Epoch::from_value(1);
      first.generation = Generation::from_value(7);
      first.revision = Revision::from_value(1);
      first.observed_at = at_seconds(1000);
      first.published_at = at_seconds(1000);
      first.value = Value::make_scalar(FixedPoint::parse("4.25").value(), Unit::kilowatts).value();
      first.provenance.origin = "selftest";
      const IngestionOutcome admitted = observatory.value().record_evidence(first);
      test.record("evidence is admitted durably",
                  admitted.code == ReasonCode::ok && admitted.durable && admitted.sequence.value() >= 1,
                  std::string(to_string(admitted.code)));

      const IngestionOutcome duplicate = observatory.value().record_evidence(first);
      test.record("duplicate evidence is idempotent", duplicate.code == ReasonCode::duplicate_evidence,
                  std::string(to_string(duplicate.code)));

      EvidenceRecord replayed = first;
      replayed.generation = Generation::from_value(6);
      const IngestionOutcome stale = observatory.value().record_evidence(replayed);
      test.record("generation fencing rejects replay", stale.code == ReasonCode::superseded_revision,
                  std::string(to_string(stale.code)));

      const Evaluation evaluation = observatory.value().evaluate(rack, draw, at_seconds(1000));
      test.record("evaluation reports known",
                  evaluation.state == ObservationState::known && evaluation.value.has_value(),
                  std::string(to_string(evaluation.state)));

      EvidenceRecord disagreement = first;
      disagreement.key.authority = bms.id;
      disagreement.generation = Generation::from_value(3);
      disagreement.value = Value::make_scalar(FixedPoint::parse("4.90").value(), Unit::kilowatts).value();
      disagreement.provenance.origin = "selftest";
      static_cast<void>(observatory.value().record_evidence(disagreement));
      const Evaluation conflicting = observatory.value().evaluate(rack, draw, at_seconds(1000));
      test.record("source disagreement is preserved as conflicting",
                  conflicting.state == ObservationState::conflicting && conflicting.disagreement.size() == 2,
                  std::string(to_string(conflicting.state)));

      auto divergences = observatory.value().divergences(at_seconds(1000));
      test.record("divergence detection finds the disagreement",
                  divergences.has_value() && !divergences.value().empty());

      auto snap = observatory.value().snapshot(at_seconds(1000));
      test.record("snapshot records a digest", snap.has_value());
      const std::string digest = observatory.value().status().digest;
      if (snap.has_value()) {
        auto rebuilt = observatory.value().reconstruct(snap.value().sequence, at_seconds(1000));
        test.record("snapshot reconstruction reproduces the digest",
                    rebuilt.has_value() && rebuilt.value().digest == digest,
                    rebuilt ? rebuilt.value().digest : std::string("reconstruction failed"));
      }
      test.record("no lock order violation was observed", LockOrderAudit::violations() == 0);
      static_cast<void>(observatory.value().close());
    }
  }

  {
    ObservatoryOptions settings;
    settings.journal.path = journal_path;
    settings.journal.origin = "selftest-reopen";
    auto observatory = Observatory::open(settings);
    test.record("journal reopens after close", observatory.has_value(),
                observatory ? std::string{} : observatory.reason().detail);
    if (observatory) {
      const EntityRef rack = EntityRef::parse("rack:sea1-r07").value();
      const AspectId draw = AspectId::parse("power.draw").value();
      const Evaluation evaluation = observatory.value().evaluate(rack, draw, at_seconds(1000));
      // The mandated invariant: recovery alone never makes a dynamic view
      // current. What it does do is keep the disagreement visible.
      test.record("recovered dynamic evidence never becomes current",
                  evaluation.state != ObservationState::known,
                  std::string(to_string(evaluation.state)) + ": " + evaluation.detail);
      test.record("recovered disagreement is still preserved",
                  evaluation.state != ObservationState::conflicting || evaluation.disagreement.size() == 2,
                  std::to_string(evaluation.disagreement.size()) + " claim(s)");
      static_cast<void>(observatory.value().close());
    }
  }

  std::filesystem::remove_all(root, error);

  JsonValue document = JsonValue::make_object();
  document.set("passed", JsonValue::make_integer(static_cast<std::int64_t>(test.passed)));
  document.set("failed", JsonValue::make_integer(static_cast<std::int64_t>(test.failed)));
  document.set("checks", std::move(test.checks));
  emit(document, pretty, 2);
  return test.failed == 0 ? ExitCode::ok : ExitCode::failure;
}

Options parse_options(int argc, char** argv) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--pretty") {
      options.pretty = true;
      continue;
    }
    if (argument == "--allow-partial" || argument == "--read-only") {
      options.values[argument].push_back("true");
      continue;
    }
    if (argument.rfind("--", 0) == 0) {
      const std::size_t equals = argument.find('=');
      if (equals != std::string::npos) {
        options.values[argument.substr(0, equals)].push_back(argument.substr(equals + 1));
        continue;
      }
      if (index + 1 < argc) {
        options.values[argument].push_back(argv[index + 1]);
        ++index;
        continue;
      }
      options.values[argument].push_back("");
      continue;
    }
    options.positional.push_back(argument);
  }
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = parse_options(argc, argv);
  if (options.positional.empty()) {
    usage();
    return static_cast<int>(ExitCode::usage);
  }
  const std::string command = options.positional.front();
  const bool pretty = options.pretty;

  if (command == "help" || command == "--help" || command == "-h") {
    usage();
    return static_cast<int>(ExitCode::ok);
  }
  if (command == "version") {
    return static_cast<int>(command_version(pretty));
  }
  if (command == "selftest") {
    return static_cast<int>(command_selftest(pretty));
  }
  if (command == "verify") {
    return static_cast<int>(command_verify(options, pretty));
  }
  if (command == "status") {
    return static_cast<int>(command_status(options, pretty));
  }
  if (command == "subjects") {
    return static_cast<int>(command_subjects(options, pretty));
  }
  if (command == "view") {
    return static_cast<int>(command_view(options, pretty));
  }
  if (command == "aggregate") {
    return static_cast<int>(command_aggregate(options, pretty));
  }
  if (command == "divergence") {
    return static_cast<int>(command_divergence(options, pretty));
  }
  if (command == "history") {
    return static_cast<int>(command_history(options, pretty));
  }
  if (command == "snapshot") {
    return static_cast<int>(command_snapshot(options, pretty));
  }
  if (command == "reconstruct") {
    return static_cast<int>(command_reconstruct(options, pretty));
  }
  if (command == "ingest") {
    return static_cast<int>(command_ingest(options, pretty));
  }

  std::fprintf(stderr, "facility-observatory: unknown command '%s'\n\n", command.c_str());
  usage();
  return static_cast<int>(ExitCode::usage);
}
