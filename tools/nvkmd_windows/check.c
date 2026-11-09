/* SPDX-License-Identifier: MIT */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>
#include "kmt.h"
#include <stdio.h>

int main(int argc, char **argv)
{
   SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
   if (argc != 2) {
      fprintf(stderr, "usage: nvk-windows-check.exe <absolute-path-to-NVK-DLL>\n");
      return 2;
   }
   struct tg_kmt_scan scan;
   LONG status = tg_gpu_kmt_scan_system(&scan);
   if (status < 0) {
      fprintf(stderr, "KMT scan failed: %08lx\n", (unsigned long)status);
      return 3;
   }
   HMODULE lib = LoadLibraryExA(argv[1], NULL,
      LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
   if (!lib) {
      fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError());
      return 4;
   }
   PFN_vkGetInstanceProcAddr gipa =
      (void *)GetProcAddress(lib, "vk_icdGetInstanceProcAddr");
   if (!gipa) { FreeLibrary(lib); return 5; }
   PFN_vkEnumerateInstanceVersion version = (void *)gipa(NULL, "vkEnumerateInstanceVersion");
   PFN_vkCreateInstance create = (void *)gipa(NULL, "vkCreateInstance");
   if (!version || !create) { FreeLibrary(lib); return 6; }
   uint32_t api = 0;
   VkResult r = version(&api);
   if (r != VK_SUCCESS || api < VK_API_VERSION_1_0) { FreeLibrary(lib); return 7; }
   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .pApplicationName = "T214 NVK instance preflight", .apiVersion = VK_API_VERSION_1_0,
   };
   const VkInstanceCreateInfo ci = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app,
   };
   VkInstance instance = VK_NULL_HANDLE;
   r = create(&ci, NULL, &instance);
   if (r != VK_SUCCESS) {
      fprintf(stderr, "vkCreateInstance failed: %d\n", r);
      FreeLibrary(lib); return 8;
   }
   PFN_vkDestroyInstance destroy = (void *)gipa(instance, "vkDestroyInstance");
   PFN_vkEnumeratePhysicalDevices enumerate = (void *)gipa(instance, "vkEnumeratePhysicalDevices");
   if (!destroy || !enumerate) { FreeLibrary(lib); return 9; }
   uint32_t count = 0;
   r = enumerate(instance, &count, NULL);
   destroy(instance, NULL);
   FreeLibrary(lib);
   printf("{\"scope\":\"NVK Windows ICD instance only\",\"api_version\":%u,"
          "\"kmt_adapters\":%u,\"private_queries_ok\":%u,\"matching_kmd\":%u,"
          "\"native_ready_kmd\":%u,\"enumerate_result\":%d,\"physical_devices\":%u,"
          "\"vulkan_draw\":false}\n",
          api, scan.enumerated, scan.queried, scan.matched, scan.native_ready, r, count);
   /* This check is for the current pre-registration milestone. Seeing no
    * device is expected, and is never counted as a native execution pass.
    */
   return r == VK_SUCCESS && !count && !scan.native_ready ? 0 : 10;
}
