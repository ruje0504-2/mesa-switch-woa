/* SPDX-License-Identifier: MIT
 * Synthetic ETW transport control. Never creates a D3D/Vulkan device or GPU work.
 */
#include "tgd3d_trace.h"
#include <cstdio>

int main()
{
   FILETIME creation = {}, exit_time = {}, kernel = {}, user = {};
   if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit_time, &kernel, &user))
      return 1;
   const uint64_t start = (uint64_t(creation.dwHighDateTime) << 32) | creation.dwLowDateTime;
   const uint64_t begin = tgd3d::qpc_now();
   tgd3d::trace(TGD3D_TRACE_VULKAN, S_OK, 0xbc22f, (uint64_t(1) << 32) | 0x403000);
   tgd3d::trace(TGD3D_TRACE_CREATE_DEVICE, S_OK, 0xa0009, 0);
   for (unsigned i = 0; i < 3; i++) {
      uint64_t ticks[4] = {tgd3d::qpc_now(), 0, 0, 0};
      uint64_t values[6] = {};
      tgd3d::trace_timing(TGD3D_TIMING_PRESENT, S_OK, i + 1, i, ticks, values);
      Sleep(1100); /* Cross real ETW buffer boundaries, without rendering. */
   }
   tgd3d::trace_message("CPU-only ETW fixture; all device and Present values are synthetic");
   std::printf("{\"pid\":%lu,\"process_start\":%llu,\"begin_qpc\":%llu,\"end_qpc\":%llu,\"gpu_work\":false}\n",
      GetCurrentProcessId(), (unsigned long long)start, (unsigned long long)begin,
      (unsigned long long)tgd3d::qpc_now());
   return 0;
}
