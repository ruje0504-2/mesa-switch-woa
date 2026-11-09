/* SPDX-License-Identifier: MIT */
#include "nvk_instance.h"
#include "vk_log.h"
#include "kmt.h"

VkResult nvk_enumerate_windows_physical_devices(struct vk_instance *instance);

VkResult
nvk_enumerate_windows_physical_devices(struct vk_instance *instance)
{
   struct tg_kmt_scan scan;
   LONG status = tg_gpu_kmt_scan_system(&scan);
   if (status < 0)
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "KMT adapter scan failed: 0x%08lx", (unsigned long)status);
   /* No diagnostic IOCTL fallback or synthetic VkPhysicalDevice. The
    * current System-class probe cannot appear in this OS adapter scan.
    * Keep device creation closed even if a future KMD enables the submit
    * bit before the Windows memory/VA/queue backend is implemented.
    */
   if (scan.native_ready)
      return vk_errorf(instance, VK_ERROR_INITIALIZATION_FAILED,
                       "Windows NVK execution backend is not enabled yet");
   return VK_SUCCESS;
}
