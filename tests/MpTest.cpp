#include "MpTest.h"

#include <chrono>
#include <cstdio>
#include <string>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/resource.h>
#include <sys/time.h>
#endif

namespace mp::test {

static double cpuSecondsNow() {
#ifdef _WIN32
  FILETIME ftCreate, ftExit, ftKernel, ftUser;
  if (GetProcessTimes(GetCurrentProcess(), &ftCreate, &ftExit, &ftKernel,
                     &ftUser)) {
    auto toSeconds = [](const FILETIME& ft) {
      ULARGE_INTEGER u;
      u.LowPart = ft.dwLowDateTime;
      u.HighPart = ft.dwHighDateTime;
      return static_cast<double>(u.QuadPart) * 1e-7; // 100ns ticks
    };
    return toSeconds(ftKernel) + toSeconds(ftUser);
  }
  return -1.0;
#else
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) == 0)
    return static_cast<double>(usage.ru_utime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec) * 1e-6 +
           static_cast<double>(usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_stime.tv_usec) * 1e-6;
  return -1.0;
#endif
}

std::vector<Test*>& registry() {
  static std::vector<Test*> instance;
  return instance;
}

Test::Test(std::string name, Category category)
  : name_(std::move(name)), category_(category) {
  registry().push_back(this);
}

Timing measure(const std::function<void()>& fn) {
  Timing t;
  const auto wallStart = std::chrono::steady_clock::now();
  const double cpuStart = cpuSecondsNow();
  fn();
  t.wallSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                wallStart)
                      .count();
  const double cpuEnd = cpuSecondsNow();
  t.cpuSeconds = (cpuStart >= 0.0 && cpuEnd >= 0.0) ? cpuEnd - cpuStart
                                                   : t.wallSeconds;
  return t;
}

int runAll(std::optional<Category> filter) {
  int failed = 0, ran = 0;
  std::printf("==================== MASTERPIECE TESTS ====================\n");
  for (Test* test : registry()) {
    if (filter && test->category() != *filter)
      continue;
    ++ran;
    try {
      test->run();
      std::printf("[PASS] %s\n", test->name().c_str());
    } catch (const Failure& f) {
      ++failed;
      std::printf("[FAIL] %s\n        %s\n", test->name().c_str(),
                  f.what().c_str());
    } catch (const std::exception& e) {
      ++failed;
      std::printf("[FAIL] %s\n        unexpected exception: %s\n",
                  test->name().c_str(), e.what());
    }
  }
  std::printf("==================== SUMMARY ====================\n");
  std::printf("%d run, %d failed\n", ran, failed);
  return failed;
}

} // namespace mp::test
