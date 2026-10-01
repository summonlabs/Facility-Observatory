// Facility Observatory - test harness.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#include "harness.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace fotest {
namespace {

std::atomic<std::uint64_t> g_counter{0};

}  // namespace

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

std::vector<std::string>& arguments() {
  static std::vector<std::string> args;
  return args;
}

void require(bool condition, const std::string& expression, const char* file, int line) {
  if (condition) {
    return;
  }
  throw Failure(std::string(file) + ":" + std::to_string(line) + ": requirement failed: " + expression);
}

void require_message(bool condition, const std::string& message, const char* file, int line) {
  if (condition) {
    return;
  }
  throw Failure(std::string(file) + ":" + std::to_string(line) + ": " + message);
}

TempDir::TempDir() {
  const auto base = std::filesystem::temp_directory_path();
  const auto stamp = static_cast<unsigned long long>(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto unique = g_counter.fetch_add(1);
  const std::string name = "fo-test-" + std::to_string(stamp) + "-" + std::to_string(unique);
  path_ = (base / name).string();
  std::error_code error;
  std::filesystem::create_directories(path_, error);
}

TempDir::~TempDir() {
  std::error_code error;
  std::filesystem::remove_all(path_, error);
}

std::string TempDir::file(const std::string& name) const { return path_ + "/" + name; }

int run_all(int argc, char** argv) {
  std::vector<std::string>& args = arguments();
  args.clear();
  for (int index = 0; index < argc; ++index) {
    args.emplace_back(argv[index]);
  }
  std::string filter;
  if (argc > 1) {
    const std::string first = argv[1];
    if (first.rfind("--filter=", 0) == 0) {
      filter = first.substr(9);
    }
  }
  std::size_t executed = 0;
  std::size_t failed = 0;
  std::vector<std::string> failures;

  for (const Case& test_case : registry()) {
    if (!filter.empty() && test_case.name.find(filter) == std::string::npos) {
      continue;
    }
    ++executed;
    try {
      test_case.body();
      std::printf("  PASS  %s\n", test_case.name.c_str());
    } catch (const Failure& failure) {
      ++failed;
      failures.push_back(test_case.name + ": " + failure.what());
      std::printf("  FAIL  %s\n        %s\n", test_case.name.c_str(), failure.what());
    } catch (const std::exception& error) {
      ++failed;
      failures.push_back(test_case.name + ": unexpected exception: " + error.what());
      std::printf("  FAIL  %s\n        unexpected exception: %s\n", test_case.name.c_str(), error.what());
    } catch (...) {
      ++failed;
      failures.push_back(test_case.name + ": unknown exception");
      std::printf("  FAIL  %s\n        unknown exception\n", test_case.name.c_str());
    }
    std::fflush(stdout);
  }

  std::printf("\n%zu case(s) executed, %zu passed, %zu failed\n", executed, executed - failed, failed);
  for (const std::string& failure : failures) {
    std::printf("FAILED: %s\n", failure.c_str());
  }
  std::fflush(stdout);
  return failed == 0 && executed > 0 ? 0 : 1;
}

}  // namespace fotest

int main(int argc, char** argv) { return fotest::run_all(argc, argv); }
