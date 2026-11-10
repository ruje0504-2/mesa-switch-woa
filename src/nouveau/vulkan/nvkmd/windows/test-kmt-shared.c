/* SPDX-License-Identifier: MIT */
/* Windows SDK ABI and ownership checks with controlled KMT substitutes. */
#include "kmt_native.h"
#include "allocation_abi.h"
#include <d3dkmthk.h>
#include <winternl.h>
#include <string.h>

#define CHECK(test) do { if (!(test)) ExitProcess(__LINE__); } while (0)

static struct {
   UINT device;
   bool alive, shared;
} allocations[32];
static UINT next_allocation, opens, destroys, residents;
static struct tg_gpu_allocation_request shared_request;
static HANDLE shared_handle;
static LONG create_status, open_status, resident_status, destroy_status;
static bool bad_query, bad_descriptor;

static UINT
new_allocation(UINT device, bool shared)
{
   CHECK(next_allocation + 1 < ARRAYSIZE(allocations));
   const UINT handle = ++next_allocation;
   allocations[handle].device = device;
   allocations[handle].alive = true;
   allocations[handle].shared = shared;
   return handle;
}

NTSTATUS APIENTRY
D3DKMTCreateAllocation2(D3DKMT_CREATEALLOCATION *args)
{
   CHECK(args->NumAllocations == 1 && args->pAllocationInfo2 &&
         !args->PrivateRuntimeDataSize && !args->PrivateDriverDataSize);
   const D3DDDI_ALLOCATIONINFO2 *info = args->pAllocationInfo2;
   CHECK(info->PrivateDriverDataSize == sizeof(shared_request));
   const struct tg_gpu_allocation_request *request = info->pPrivateDriverData;
   CHECK(request->version == TG_GPU_ALLOCATION_ABI && request->bytes == 8192 &&
         request->flags == TG_GPU_ALLOCATION_CPU_VISIBLE);
   if (create_status)
      return create_status;
   const bool shared = args->Flags.CreateShared != 0;
   CHECK(args->Flags.CreateResource == shared && args->Flags.NtSecuritySharing == shared);
   args->pAllocationInfo2->hAllocation = new_allocation(args->hDevice, shared);
   if (shared) {
      args->hResource = args->pAllocationInfo2->hAllocation + 100;
      shared_request = *request;
   }
   return 0;
}

NTSTATUS APIENTRY
D3DKMTDestroyAllocation2(const D3DKMT_DESTROYALLOCATION2 *args)
{
   UINT handle;
   if (args->hResource) {
      CHECK(!args->AllocationCount && !args->phAllocationList);
      handle = args->hResource - 100;
      CHECK(handle <= next_allocation && allocations[handle].shared);
   } else {
      CHECK(args->AllocationCount == 1 && args->phAllocationList);
      handle = args->phAllocationList[0];
      CHECK(handle <= next_allocation && !allocations[handle].shared);
   }
   CHECK(handle && allocations[handle].alive && allocations[handle].device == args->hDevice);
   if (destroy_status)
      return destroy_status;
   allocations[handle].alive = false;
   destroys++;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTMakeResident(D3DDDI_MAKERESIDENT *args)
{
   CHECK(args->NumAllocations == 1 && args->AllocationList &&
         allocations[args->AllocationList[0]].alive);
   residents++;
   args->PagingFenceValue = 7;
   return resident_status;
}

NTSTATUS APIENTRY
D3DKMTWaitForSynchronizationObjectFromCpu(const D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU *args)
{
   (void)args;
   /* The fixture's completed paging fence already covers every request. */
   return TG_KMT_INVALID_PARAMETER;
}

NTSTATUS APIENTRY
D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *objects,
                   POBJECT_ATTRIBUTES attributes, DWORD access, HANDLE *handle)
{
   CHECK(count == 1 && *objects > 100 && allocations[*objects - 100].alive);
   CHECK(attributes->Length == sizeof(*attributes) && !attributes->ObjectName &&
         access == (STANDARD_RIGHTS_READ | SHARED_ALLOCATION_WRITE));
   *handle = CreateEventW(NULL, FALSE, FALSE, NULL);
   CHECK(*handle);
   shared_handle = *handle;
   return 0;
}

NTSTATUS APIENTRY
D3DKMTQueryResourceInfoFromNtHandle(D3DKMT_QUERYRESOURCEINFOFROMNTHANDLE *args)
{
   DWORD flags;
   CHECK(args->hNtHandle == shared_handle && GetHandleInformation(shared_handle, &flags));
   args->NumAllocations = bad_query ? 2 : 1;
   args->TotalPrivateDriverDataSize = sizeof(shared_request);
   return 0;
}

NTSTATUS APIENTRY
D3DKMTOpenResourceFromNtHandle(D3DKMT_OPENRESOURCEFROMNTHANDLE *args)
{
   CHECK(args->hNtHandle == shared_handle && args->NumAllocations == 1 &&
         !args->PrivateRuntimeDataSize && !args->ResourcePrivateDriverDataSize &&
         args->TotalPrivateDriverDataBufferSize == sizeof(shared_request));
   opens++;
   if (open_status)
      return open_status;
   args->pOpenAllocationInfo2->hAllocation = new_allocation(args->hDevice, true);
   args->hResource = args->pOpenAllocationInfo2->hAllocation + 100;
   args->pOpenAllocationInfo2->PrivateDriverDataSize = sizeof(shared_request);
   args->pOpenAllocationInfo2->pPrivateDriverData = args->pTotalPrivateDriverDataBuffer;
   struct tg_gpu_allocation_request *request = args->pTotalPrivateDriverDataBuffer;
   *request = shared_request;
   if (bad_descriptor)
      request->version++;
   return 0;
}

int main(void)
{
   const volatile uint64_t completed = 7;
   struct tg_kmt_device producer = {
      .handle = 1, .paging_queue = 3, .paging = {.value = &completed}, .budget = 65536,
   };
   struct tg_kmt_device consumer = {
      .handle = 2, .paging_queue = 4, .paging = {.value = &completed}, .budget = 65536,
   };
   InitializeSRWLock(&producer.memory_lock);
   InitializeSRWLock(&consumer.memory_lock);
   struct tg_kmt_allocation original, imported, second;
   HANDLE handle;
   DWORD flags;

   CHECK(tg_kmt_allocate(&producer, 4097, true, &original) == 0);
   CHECK(original.size == 8192 && !original.resource && producer.allocated == 8192);
   CHECK(tg_kmt_export_memory(&original, &handle) == TG_KMT_INVALID_PARAMETER && !handle);
   CHECK(tg_kmt_free(&producer, &original) == 0 && !producer.allocated && !original.handle);
   CHECK(tg_kmt_allocate_shared(&producer, UINT64_MAX, true, &original) == TG_KMT_NO_VIDEO_MEMORY);
   create_status = TG_KMT_NO_MEMORY;
   CHECK(tg_kmt_allocate_shared(&producer, 4097, true, &original) == create_status);
   CHECK(!producer.allocated && !original.handle);
   create_status = 0;
   CHECK(tg_kmt_allocate_shared(&producer, 4097, true, &original) == 0);
   CHECK(original.resource && original.flags == TG_GPU_ALLOCATION_CPU_VISIBLE);
   CHECK(tg_kmt_export_memory(&original, &handle) == 0 && GetHandleInformation(handle, &flags));
   CHECK(tg_kmt_import_memory(&consumer, handle, &imported) == 0);
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == 0);
   CHECK(imported.handle != original.handle && imported.handle != second.handle &&
         imported.resource != original.resource && imported.resource != second.resource &&
         imported.size == original.size && imported.flags == original.flags &&
         consumer.allocated == 16384 && producer.allocated == 8192);

   /* The NT handle preserves the payload after the exporter releases its
    * local resource. Each subsequent import has independent ownership. */
   CHECK(tg_kmt_free(&producer, &original) == 0 && !producer.allocated);
   CHECK(GetHandleInformation(handle, &flags));
   CHECK(tg_kmt_free(&consumer, &second) == 0 && consumer.allocated == 8192);
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == 0);
   CHECK(tg_kmt_free(&consumer, &second) == 0);

   const UINT before_opens = opens, before_destroys = destroys;
   bad_query = true;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_INVALID_PARAMETER &&
         !second.handle && opens == before_opens && destroys == before_destroys);
   bad_query = false;
   bad_descriptor = true;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_INVALID_PARAMETER &&
         !second.handle && destroys == before_destroys + 1 && consumer.allocated == 8192);
   bad_descriptor = false;
   consumer.budget = consumer.allocated;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == TG_KMT_NO_VIDEO_MEMORY &&
         !second.handle && destroys == before_destroys + 2 && consumer.allocated == 8192);
   consumer.budget = 65536;
   open_status = TG_KMT_NO_MEMORY;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == open_status &&
         !second.handle && consumer.allocated == 8192);
   open_status = 0;
   resident_status = TG_KMT_NO_VIDEO_MEMORY;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == resident_status &&
         !second.handle && destroys == before_destroys + 3 && consumer.allocated == 8192);
   destroy_status = TG_KMT_DEVICE_REMOVED;
   CHECK(tg_kmt_import_memory(&consumer, handle, &second) == resident_status &&
         second.resource && second.size == 8192 && consumer.allocated == 16384);
   resident_status = destroy_status = 0;
   CHECK(tg_kmt_free(&consumer, &second) == 0 && consumer.allocated == 8192);
   CHECK(GetHandleInformation(handle, &flags) && CloseHandle(handle));
   CHECK(tg_kmt_free(&consumer, &imported) == 0 && !consumer.allocated);
   CHECK(residents >= 6 && destroys == next_allocation);
   for (UINT i = 1; i <= next_allocation; i++)
      CHECK(!allocations[i].alive);
   const char pass[] = "PASS: SDK shared-resource handles, repeated imports, budgets and failed-residency cleanup (controlled KMT)\r\n";
   DWORD written;
   CHECK(WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), pass, sizeof(pass) - 1, &written, NULL));
   return 0;
}
