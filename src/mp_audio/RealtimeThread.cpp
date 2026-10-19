#include "RealtimeThread.h"

#include <algorithm>
#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#elif defined(__linux__) && !defined(__ANDROID__)
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace mp::realtime {

#if defined(_WIN32)

// The thread's MMCSS registration, kept to undo it.
static thread_local HANDLE mmcss = nullptr;

// Hybrid processors (Intel since the 12th generation) put a thread the
// system judges unimportant on the efficiency cores, at a lower clock.
// Asked for, never: a render thread that runs slow makes the block late.
static void throttleForPower(bool allowed) {
#ifdef THREAD_POWER_THROTTLING_CURRENT_VERSION
  THREAD_POWER_THROTTLING_STATE state{};
  state.Version = THREAD_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = allowed ? 0 : THREAD_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = 0;
  SetThreadInformation(GetCurrentThread(), ThreadPowerThrottling, &state, sizeof(state));
#else
  (void)allowed;
#endif
}

bool supported() { return true; }
const char* method() { return "MMCSS Pro Audio"; }

bool makeRealtime(double) {
  if (mmcss == nullptr) {
    DWORD task = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
  }
  throttleForPower(false);
  return mmcss != nullptr;
}

void makeNormal() {
  if (mmcss != nullptr) AvRevertMmThreadCharacteristics(mmcss);
  mmcss = nullptr;
  throttleForPower(true);
}

// JUCE puts its WASAPI thread in MMCSS itself, and an ASIO driver runs its
// own thread.
void setAudioThread(bool, double) {}

#elif defined(__APPLE__)

static thread_local bool timeConstrained = false;

bool supported() { return true; }
const char* method() { return "Mach time constraint"; }

// A share may take up to a block, as JUCE asks of a thread made with the
// audio processing time. Only a thread like this can join the audio
// device's workgroup.
bool makeRealtime(double blockSeconds) {
  mach_timebase_info_data_t base{};
  mach_timebase_info(&base);
  const double seconds = std::clamp(blockSeconds, 0.0005, 0.05);
  const double ticks = seconds * 1e9 * static_cast<double>(base.denom) / static_cast<double>(base.numer);
  thread_time_constraint_policy_data_t policy;
  policy.period = 0;
  policy.computation = static_cast<uint32_t>(ticks);
  policy.constraint = policy.computation;
  policy.preemptible = true;
  timeConstrained = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                                      reinterpret_cast<thread_policy_t>(&policy),
                                      THREAD_TIME_CONSTRAINT_POLICY_COUNT) == KERN_SUCCESS;
  return timeConstrained;
}

void makeNormal() {
  if (!timeConstrained) return;
  thread_standard_policy_data_t policy{};
  thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY,
                    reinterpret_cast<thread_policy_t>(&policy), THREAD_STANDARD_POLICY_COUNT);
  timeConstrained = false;
}

// Core Audio's own thread is real-time already.
void setAudioThread(bool, double) {}

#elif defined(__linux__) && !defined(__ANDROID__)

// What the thread had before, to give it back.
struct Saved {
  bool changed = false;
  int policy = SCHED_OTHER;
  sched_param param{};
};
static thread_local Saved saved;

// Above every ordinary thread and below the kernel's interrupt threads (50
// on a real-time kernel). A user may have less: the audio group's limit, or
// none at all, and then the system says no.
static int priority() {
  constexpr int kWanted = 40;
  if (geteuid() == 0) return kWanted;
  rlimit limit{};
  if (getrlimit(RLIMIT_RTPRIO, &limit) != 0) return 0;
  if (limit.rlim_cur == RLIM_INFINITY) return kWanted;
  return static_cast<int>(std::min<rlim_t>(limit.rlim_cur, kWanted));
}

bool supported() { return true; }
const char* method() { return "SCHED_FIFO"; }

bool makeRealtime(double) {
  const int p = priority();
  if (p <= 0) return false;
  Saved before;
  if (!saved.changed && pthread_getschedparam(pthread_self(), &before.policy, &before.param) != 0)
    return false;
  sched_param param{};
  param.sched_priority = p;
  if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) return false;
  if (!saved.changed) {
    saved = before;
    saved.changed = true;
  }
  return true;
}

void makeNormal() {
  if (!saved.changed) return;
  pthread_setschedparam(pthread_self(), saved.policy, &saved.param);
  saved.changed = false;
}

void setAudioThread(bool on, double blockSeconds) {
  if (!on) {
    makeNormal();
    return;
  }
  int policy = SCHED_OTHER;
  sched_param param{};
  if (pthread_getschedparam(pthread_self(), &policy, &param) != 0) return;
  // JACK's thread, real-time at the priority the server chose: left so.
  if (!saved.changed && (policy == SCHED_FIFO || policy == SCHED_RR)) return;
  makeRealtime(blockSeconds);
}

#else

bool supported() { return false; }
const char* method() { return ""; }
bool makeRealtime(double) { return false; }
void makeNormal() {}
void setAudioThread(bool, double) {}

#endif

} // namespace mp::realtime
