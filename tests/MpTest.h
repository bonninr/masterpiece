// Masterpiece test framework — FUNCTIONAL/PERF categories, one exe,
// two CTest names (MpFunctional --no-perf / MpPerf --perf-only).
// Architecture ported from GrandOrgue's GOTestCollection CI pattern
// (src/tests/, .github/workflows/build.yml "tests" job).
#pragma once
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mp::test {

enum class Category { Functional, Perf };

class Failure {
public:
  explicit Failure(std::string msg) : msg_(std::move(msg)) {}
  const std::string& what() const { return msg_; }
private:
  std::string msg_;
};

struct Timing {
  double wallSeconds = 0.0;
  double cpuSeconds = 0.0; // rusage (POSIX) / GetProcessTimes (Win32)
};

class Test {
public:
  Test(std::string name, Category category);
  virtual ~Test() = default;
  virtual void run() = 0; // throws Failure

  const std::string& name() const { return name_; }
  Category category() const { return category_; }

private:
  std::string name_;
  Category category_;
};

// Self-registration: static instances in test TUs land here in their ctor.
std::vector<Test*>& registry();

// Runs registry (optionally filtered). Prints per-test result + summary.
// Returns failed count (0 = success).
int runAll(std::optional<Category> filter);

// Measures fn() once; wall via steady_clock, CPU via rusage/GetProcessTimes.
Timing measure(const std::function<void()>& fn);

} // namespace mp::test

#define MP_CHECK(cond, msg)                                                   \
  do {                                                                       \
    if (!(cond))                                                             \
      throw ::mp::test::Failure(std::string(__FILE__) + ":" +               \
                                std::to_string(__LINE__) + ": " + (msg));   \
  } while (0)
