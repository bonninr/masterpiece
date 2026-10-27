#include "SystemMemory.h"

#include <juce_core/juce_core.h>

#include <algorithm>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <psapi.h>
#elif defined(__APPLE__)
 #include <TargetConditionals.h>
 #include <mach/mach.h>
 // A device reports the memory left before the system ends the process. The
 // simulator sets no such limit, and reports 0 there: it runs on the Mac, and
 // the Mac's own figures apply.
 #if TARGET_OS_IPHONE && !TARGET_OS_SIMULATOR
  #define MP_PROCESS_MEMORY_LIMIT 1
  #include <os/proc.h>
 #endif
#elif defined(__linux__)
 #include <cstdio>
 #include <cstring>
#endif

namespace mp {

#if defined(MP_PROCESS_MEMORY_LIMIT)
// What this process holds as the system counts it: the figure iOS kills by.
static int64_t footprintBytes() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) !=
      KERN_SUCCESS)
    return -1;
  return static_cast<int64_t>(info.phys_footprint);
}
#endif

int64_t availableMemoryBytes() {
#if defined(_WIN32)
  MEMORYSTATUSEX s{};
  s.dwLength = sizeof(s);
  if (!GlobalMemoryStatusEx(&s)) return -1;
  // Past the free physical memory the samples are paged to disk and play
  // with gaps; past the commit, an allocation fails and the program ends.
  int64_t free = static_cast<int64_t>(std::min(s.ullAvailPhys, s.ullAvailPageFile));
  // Neither figure is enough alone. Windows counts its file cache as free
  // memory and grows the page file to raise the commit, so a load read from
  // a disk kept both healthy while this program's own samples were paged
  // out: a 2009 surround set went on loading into a page file that filled
  // the system disk. What this process holds beyond what is in memory has
  // been paged out, and it comes off the figure.
  PROCESS_MEMORY_COUNTERS_EX pmc{};
  if (GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc),
                           sizeof(pmc)) &&
      pmc.PrivateUsage > pmc.WorkingSetSize)
    free -= static_cast<int64_t>(pmc.PrivateUsage - pmc.WorkingSetSize);
  return free;
#elif defined(MP_PROCESS_MEMORY_LIMIT)
  return static_cast<int64_t>(os_proc_available_memory());
#elif defined(__APPLE__)
  vm_statistics64_data_t vm{};
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm),
                        &count) != KERN_SUCCESS)
    return -1;
  vm_size_t page = 0;
  if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS) return -1;
  const uint64_t pages = static_cast<uint64_t>(vm.free_count) + vm.inactive_count +
                         vm.purgeable_count + vm.speculative_count;
  return static_cast<int64_t>(pages * page);
#elif defined(__linux__)
  // Android included: the kernel's own reckoning of what can be had without
  // swapping, the page cache it can drop counted in.
  std::FILE* f = std::fopen("/proc/meminfo", "r");
  if (f == nullptr) return -1;
  char line[256];
  int64_t kb = -1;
  while (std::fgets(line, sizeof(line), f) != nullptr)
    if (std::strncmp(line, "MemAvailable:", 13) == 0) {
      long long v = 0;
      if (std::sscanf(line + 13, "%lld", &v) == 1) kb = v;
      break;
    }
  std::fclose(f);
  return kb < 0 ? -1 : kb * 1024;
#else
  return -1;
#endif
}

int64_t processMemoryCeilingBytes() {
#if defined(MP_PROCESS_MEMORY_LIMIT)
  const int64_t held = footprintBytes();
  if (held < 0) return -1;
  return held + static_cast<int64_t>(os_proc_available_memory());
#else
  return -1;
#endif
}

int64_t memoryReserveBytes() {
  const int64_t ceiling = processMemoryCeilingBytes();
  const int64_t total =
      ceiling > 0 ? ceiling
                  : static_cast<int64_t>(juce::SystemStats::getMemorySizeInMegabytes()) * 1024 * 1024;
  return std::max<int64_t>(int64_t{256} * 1024 * 1024, total / 20);
}

}  // namespace mp
