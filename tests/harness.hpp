// Facility Observatory - test harness.
//
// Deliberately dependency free: the suite must build from a fresh clone with no
// network access. Failures stop the current case and are reported with the
// expression, file and line that produced them.
//
// Copyright 2026 Summon Software Labs.
// SPDX-License-Identifier: Apache-2.0

#ifndef FACILITY_OBSERVATORY_TESTS_HARNESS_HPP
#define FACILITY_OBSERVATORY_TESTS_HARNESS_HPP

#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fotest {

class Failure : public std::exception {
 public:
  explicit Failure(std::string message) : message_(std::move(message)) {}
  [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

 private:
  std::string message_;
};

struct Case {
  std::string name;
  std::function<void()> body;
};

std::vector<Case>& registry();

// The test executable's own argv, so a suite can locate sibling binaries
// (the CLI, the multiprocess child) without a hard-coded path.
std::vector<std::string>& arguments();
void require(bool condition, const std::string& expression, const char* file, int line);
void require_message(bool condition, const std::string& message, const char* file, int line);

int run_all(int argc, char** argv);

struct Registrar {
  Registrar(const char* name, std::function<void()> body) { registry().push_back(Case{name, std::move(body)}); }
};

// Deterministic PRNG: splitmix64. Every randomized test states its seed and can
// be replayed exactly.
class Rng {
 public:
  explicit Rng(std::uint64_t seed) : state_(seed) {}

  std::uint64_t next() {
    state_ += 0x9E3779B97F4A7C15ULL;
    std::uint64_t z = state_;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
  }

  std::uint64_t below(std::uint64_t bound) { return bound == 0 ? 0 : next() % bound; }

  std::int64_t range(std::int64_t low, std::int64_t high) {
    if (high <= low) {
      return low;
    }
    return low + static_cast<std::int64_t>(below(static_cast<std::uint64_t>(high - low + 1)));
  }

  bool coin() { return (next() & 1ULL) != 0ULL; }

 private:
  std::uint64_t state_;
};

// A directory under the system temporary directory that removes itself.
class TempDir {
 public:
  TempDir();
  ~TempDir();
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;

  [[nodiscard]] const std::string& path() const noexcept { return path_; }
  [[nodiscard]] std::string file(const std::string& name) const;

 private:
  std::string path_;
};

// Renders any streamable value for a failure message.
template <class T>
std::string describe(const T& value) {
  std::ostringstream out;
  out << value;
  return out.str();
}

template <class A, class B>
void require_equal(const A& actual, const B& expected, const std::string& expression, const char* file, int line) {
  if (!(actual == expected)) {
    require(false, expression + ": expected " + describe(expected) + ", got " + describe(actual), file, line);
  }
}

}  // namespace fotest

#define FO_TEST(name)                                                                     \
  static void fo_case_##name();                                                           \
  static const ::fotest::Registrar fo_registrar_##name(#name, fo_case_##name);            \
  static void fo_case_##name()

#define FO_REQUIRE(expression) ::fotest::require((expression), #expression, __FILE__, __LINE__)

#define FO_REQUIRE_MESSAGE(condition, message) \
  ::fotest::require_message((condition), (message), __FILE__, __LINE__)

#define FO_REQUIRE_EQ(actual, expected) \
  ::fotest::require_equal((actual), (expected), #actual " == " #expected, __FILE__, __LINE__)

#endif  // FACILITY_OBSERVATORY_TESTS_HARNESS_HPP
