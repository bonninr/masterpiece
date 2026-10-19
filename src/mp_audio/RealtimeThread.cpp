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
#include <dlfcn.h>
#include <mutex>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
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

// rtkit, the service desktops run to hand out real-time priority on request
// (PipeWire takes its own that way), for a user the limits give none; inside
// the Flatpak, the desktop portal in front of it. Reached over D-Bus with
// sd-bus, loaded when first needed, so the program does not depend on it and
// runs as before where it is missing.
struct BusError {
  const char* name = nullptr;
  const char* message = nullptr;
  int needFree = 0;
};
using BusOpen = int (*)(void**);
using BusCall = int (*)(void*, const char*, const char*, const char*, const char*, BusError*, void**,
                        const char*, ...);
using BusGet = int (*)(void*, const char*, const char*, const char*, const char*, BusError*, char, void*);
using BusUnref = void* (*)(void*);
using BusErrorFree = void (*)(BusError*);

struct Kit {
  bool tried = false;
  void* bus = nullptr;
  const char* destination = nullptr;
  const char* path = nullptr;
  const char* interface = nullptr;
  const char* name = nullptr;
  int maxPriority = 0;
  int64_t rttimeMax = 0; // microseconds
  BusCall call = nullptr;
  BusUnref unrefMessage = nullptr;
  BusErrorFree freeError = nullptr;
};
// One bus connection for every thread, and sd-bus wants one caller at a time.
static std::mutex kitMutex;
static Kit kit;

// Found once; nullptr when there is none, or it gives nothing.
static Kit* findKit() {
  if (kit.tried) return kit.bus != nullptr ? &kit : nullptr;
  kit.tried = true;
  void* lib = dlopen("libsystemd.so.0", RTLD_NOW | RTLD_LOCAL);
  if (lib == nullptr) return nullptr;
  const auto openSystem = reinterpret_cast<BusOpen>(dlsym(lib, "sd_bus_open_system"));
  const auto openUser = reinterpret_cast<BusOpen>(dlsym(lib, "sd_bus_open_user"));
  const auto get = reinterpret_cast<BusGet>(dlsym(lib, "sd_bus_get_property_trivial"));
  const auto unrefBus = reinterpret_cast<BusUnref>(dlsym(lib, "sd_bus_unref"));
  kit.call = reinterpret_cast<BusCall>(dlsym(lib, "sd_bus_call_method"));
  kit.unrefMessage = reinterpret_cast<BusUnref>(dlsym(lib, "sd_bus_message_unref"));
  kit.freeError = reinterpret_cast<BusErrorFree>(dlsym(lib, "sd_bus_error_free"));
  if (openSystem == nullptr || openUser == nullptr || get == nullptr || unrefBus == nullptr ||
      kit.call == nullptr || kit.unrefMessage == nullptr || kit.freeError == nullptr)
    return nullptr;

  const bool sandboxed = access("/.flatpak-info", F_OK) == 0;
  if (sandboxed) {
    kit.destination = "org.freedesktop.portal.Desktop";
    kit.path = "/org/freedesktop/portal/desktop";
    kit.interface = "org.freedesktop.portal.Realtime";
    kit.name = "the realtime portal";
  } else {
    kit.destination = "org.freedesktop.RealtimeKit1";
    kit.path = "/org/freedesktop/RealtimeKit1";
    kit.interface = "org.freedesktop.RealtimeKit1";
    kit.name = "rtkit";
  }
  void* bus = nullptr;
  if ((sandboxed ? openUser : openSystem)(&bus) < 0 || bus == nullptr) return nullptr;
  int32_t maxPriority = 0;
  int64_t rttimeMax = 0;
  BusError e1, e2;
  const bool read = get(bus, kit.destination, kit.path, kit.interface, "MaxRealtimePriority", &e1, 'i',
                        &maxPriority) >= 0 &&
                    get(bus, kit.destination, kit.path, kit.interface, "RTTimeUSecMax", &e2, 'x',
                        &rttimeMax) >= 0;
  kit.freeError(&e1);
  kit.freeError(&e2);
  if (!read || maxPriority <= 0 || rttimeMax <= 0) {
    unrefBus(bus);
    return nullptr;
  }
  kit.bus = bus;
  kit.maxPriority = maxPriority;
  kit.rttimeMax = rttimeMax;
  return &kit;
}

// The calling thread, real-time through the kit. Its condition: a limit on
// the CPU time a real-time thread may take without sleeping, no more than it
// allows (200 ms by default; the kernel stops a program that passes it). A
// render thread sleeps between blocks, and the ALSA thread in its wait for the
// device, so neither comes near it.
static bool askKit(int wanted) {
  const std::lock_guard<std::mutex> lock(kitMutex);
  Kit* const k = findKit();
  if (k == nullptr) return false;
  rlimit rttime{};
  if (getrlimit(RLIMIT_RTTIME, &rttime) != 0) return false;
  const auto allowed = static_cast<rlim_t>(k->rttimeMax);
  if (rttime.rlim_max == RLIM_INFINITY || rttime.rlim_max > allowed) {
    const rlimit limit{allowed, allowed};
    if (setrlimit(RLIMIT_RTTIME, &limit) != 0) return false;
  }
  BusError error;
  void* reply = nullptr;
  const auto pid = static_cast<uint64_t>(getpid());
  const auto tid = static_cast<uint64_t>(syscall(SYS_gettid));
  const auto priority = static_cast<uint32_t>(std::min(wanted, k->maxPriority));
  const int r = k->call(k->bus, k->destination, k->path, k->interface, "MakeThreadRealtimeWithPID", &error,
                        &reply, "ttu", pid, tid, priority);
  if (reply != nullptr) k->unrefMessage(reply);
  k->freeError(&error);
  return r >= 0;
}

bool supported() { return true; }

const char* method() {
  if (priority() > 0) return "SCHED_FIFO";
  const std::lock_guard<std::mutex> lock(kitMutex);
  const Kit* const k = findKit();
  return k != nullptr ? k->name : "";
}

bool makeRealtime(double) {
  Saved before;
  if (!saved.changed && pthread_getschedparam(pthread_self(), &before.policy, &before.param) != 0)
    return false;
  const int p = priority();
  bool done = false;
  if (p > 0) {
    sched_param param{};
    param.sched_priority = p;
    done = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) == 0;
  } else {
    done = askKit(40);
  }
  if (!done) return false;
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
