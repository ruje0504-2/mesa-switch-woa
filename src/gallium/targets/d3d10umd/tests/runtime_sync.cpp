/* SPDX-License-Identifier: MIT */
/* Actual runtime bridge with DDI/KMT/Gallium substitutes, no GPU execution. */
#include "RuntimeMemory.h"
#include "State.h"
#include <d3dkmthk.h>
#include <winternl.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %u: %s\n", __LINE__, #x); exit(1); } } while (0)

struct FakeFence { unsigned refs; } fences[2], completion;
static Device device;
static pipe_context pipe;
static pipe_screen screen = {};
static pipe_submission_sync bridge;
static HANDLE handles[2];
static unsigned creates, shares, imports, live_objects, contexts, flushes, cpu_waits;
static unsigned signals, waits, fail_create, fail_share, fail_import;
static uint64_t value;
static bool fail_register, fail_signal, fail_wait, fail_submit;
static HANDLE rt_device = reinterpret_cast<HANDLE>(0x1230);
static HANDLE rt_context = reinterpret_cast<HANDLE>(0x4560);

extern "C" void D3D10Trace(d3d10_umd_trace_event event, HRESULT, uint64_t, uint64_t)
{
   CHECK(event == D3D10_TRACE_RUNTIME_CONTEXT);
}

extern "C" NTSTATUS APIENTRY
D3DKMTShareObjects(UINT count, const D3DKMT_HANDLE *objects,
                   POBJECT_ATTRIBUTES attributes, DWORD access, HANDLE *out)
{
   ++shares;
   CHECK(count == 1 && *objects == 0x80 + shares);
   CHECK(attributes && attributes->Length == sizeof(*attributes));
   CHECK(!attributes->RootDirectory && !attributes->ObjectName && !attributes->Attributes);
   CHECK(access == D3DDDI_SYNC_OBJECT_ALL_ACCESS);
   if (shares == fail_share) return static_cast<NTSTATUS>(0xc0000022);
   *out = handles[shares - 1] = CreateEventW(NULL, TRUE, FALSE, NULL);
   CHECK(*out);
   return 0;
}

static void reference(pipe_screen *, pipe_fence_handle **dst, pipe_fence_handle *src)
{
   auto from = reinterpret_cast<FakeFence *>(src);
   auto to = reinterpret_cast<FakeFence *>(*dst);
   if (from) ++from->refs;
   if (to) { CHECK(to->refs); --to->refs; }
   *dst = src;
}

static bool set_sync(pipe_context *, const pipe_submission_sync *sync)
{
   if (sync && fail_register) return false;
   reference(&screen, &bridge.wait_fence, NULL);
   reference(&screen, &bridge.signal_fence, NULL);
   bridge = {};
   if (sync) {
      CHECK(!sync->initial_value && sync->prepare && sync->submitted && sync->data);
      bridge = *sync;
      bridge.wait_fence = bridge.signal_fence = NULL;
      reference(&screen, &bridge.wait_fence, sync->wait_fence);
      reference(&screen, &bridge.signal_fence, sync->signal_fence);
   }
   return true;
}

static void flush(pipe_context *, pipe_fence_handle **out, unsigned)
{
   ++flushes;
   CHECK(bridge.prepare && bridge.submitted);
   if (!bridge.prepare(bridge.data, ++value)) return;
   if (!bridge.submitted(bridge.data, value, !fail_submit)) return;
   if (out) reference(&screen, out, reinterpret_cast<pipe_fence_handle *>(&completion));
}

static void init()
{
   CHECK(!live_objects && !contexts && !fences[0].refs && !fences[1].refs && !completion.refs);
   device = {}; pipe = {}; bridge = {};
   handles[0] = handles[1] = NULL;
   creates = shares = imports = flushes = cpu_waits = signals = waits = 0;
   fail_create = fail_share = fail_import = 0;
   fail_register = fail_signal = fail_wait = fail_submit = false;
   value = 0;
   device.hDevice = rt_device; device.pipe = &pipe; pipe.screen = &screen;
   pipe.set_submission_sync = set_sync;
   pipe.flush = flush;
   pipe.get_device_reset_status = [](pipe_context *) { return PIPE_NO_RESET; };
   screen.fence_reference = reference;
   screen.fence_finish = [](pipe_screen *, pipe_context *, pipe_fence_handle *f, uint64_t timeout) {
      CHECK(f == reinterpret_cast<pipe_fence_handle *>(&completion) && timeout == UINT64_MAX);
      ++cpu_waits; return true;
   };
   screen.create_fence_win32 = [](pipe_screen *, pipe_fence_handle **out, void *handle,
                                  const void *name, pipe_fd_type type) {
      ++imports;
      DWORD flags;
      CHECK(!name && type == PIPE_FD_TYPE_TIMELINE_SEMAPHORE_D3D12);
      CHECK(GetHandleInformation(handle, &flags) && !(flags & HANDLE_FLAG_INHERIT));
      if (imports == fail_import) return;
      CHECK(!*out && !fences[imports - 1].refs);
      fences[imports - 1].refs = 1;
      *out = reinterpret_cast<pipe_fence_handle *>(&fences[imports - 1]);
   };
   auto &cb = device.KTCallbacks;
   cb.pfnCreateContextVirtualCb = [](HANDLE d, D3DDDICB_CREATECONTEXTVIRTUAL *args) -> HRESULT {
      CHECK(d == rt_device && args->EngineAffinity == 1 && !contexts);
      args->hContext = rt_context; ++contexts; return S_OK;
   };
   cb.pfnDestroyContextCb = [](HANDLE d, const D3DDDICB_DESTROYCONTEXT *args) -> HRESULT {
      CHECK(d == rt_device && args->hContext == rt_context && contexts == 1);
      CHECK(!live_objects && !bridge.prepare);
      --contexts; return S_OK;
   };
   cb.pfnCreateSynchronizationObject2Cb = [](HANDLE d, D3DDDICB_CREATESYNCHRONIZATIONOBJECT2 *args) -> HRESULT {
      ++creates;
      CHECK(d == rt_device && args->Info.Type == D3DDDI_MONITORED_FENCE);
      CHECK(args->Info.Flags.Shared && args->Info.Flags.NtSecuritySharing);
      CHECK(!args->Info.MonitoredFence.InitialFenceValue);
      CHECK(args->Info.MonitoredFence.EngineAffinity == 1);
      if (creates == fail_create) return E_OUTOFMEMORY;
      args->hSyncObject = 0x80 + creates; ++live_objects; return S_OK;
   };
   cb.pfnDestroySynchronizationObjectCb = [](HANDLE d, const D3DDDICB_DESTROYSYNCHRONIZATIONOBJECT *args) -> HRESULT {
      CHECK(d == rt_device && args->hSyncObject >= 0x81 && args->hSyncObject <= 0x82 && live_objects);
      CHECK(!fences[args->hSyncObject - 0x81].refs);
      --live_objects; return S_OK;
   };
   cb.pfnSignalSynchronizationObjectFromGpuCb = [](HANDLE d, const D3DDDICB_SIGNALSYNCHRONIZATIONOBJECTFROMGPU *args) -> HRESULT {
      CHECK(d == rt_device && args->hContext == rt_context && args->ObjectCount == 1);
      CHECK(args->ObjectHandleArray[0] == 0x81 && args->MonitoredFenceValueArray[0] == value);
      ++signals; return fail_signal ? E_FAIL : S_OK;
   };
   cb.pfnWaitForSynchronizationObjectFromGpuCb = [](HANDLE d, const D3DDDICB_WAITFORSYNCHRONIZATIONOBJECTFROMGPU *args) -> HRESULT {
      CHECK(d == rt_device && args->hContext == rt_context && args->ObjectCount == 1);
      CHECK(args->ObjectHandleArray[0] == 0x82 && args->MonitoredFenceValueArray[0] == value);
      CHECK(signals > waits && !fail_signal && !fail_submit);
      ++waits; return fail_wait ? E_FAIL : S_OK;
   };
}

static void check_clean()
{
   CHECK(!device.hContext && !device.runtime_sync && !live_objects && !contexts);
   CHECK(!fences[0].refs && !fences[1].refs && !completion.refs && !bridge.prepare);
   DWORD flags;
   for (HANDLE h : handles) if (h) CHECK(!GetHandleInformation(h, &flags));
}

int main()
{
   init();
   CHECK(RuntimeDeviceCreateContext(&device) == S_OK && live_objects == 2);
   CHECK(fences[0].refs == 2 && fences[1].refs == 2);
   CHECK(RuntimeDeviceFlush(&device) == S_OK && signals == 1 && waits == 1 && !cpu_waits);
   /* A driver's automatic batch uses the same registered callbacks. */
   flush(&pipe, NULL, 0);
   CHECK(signals == 2 && waits == 2 && !cpu_waits);
   CHECK(RuntimeDeviceFinish(&device) == S_OK && signals == 3 && waits == 3 && cpu_waits == 1);
   CHECK(RuntimeDeviceDestroyContext(&device) == S_OK);
   check_clean();

   for (unsigned failure = 0; failure < 7; ++failure) {
      init();
      if (failure < 2) fail_create = failure + 1;
      else if (failure < 4) fail_share = failure - 1;
      else if (failure < 6) fail_import = failure - 3;
      else fail_register = true;
      CHECK(FAILED(RuntimeDeviceCreateContext(&device)));
      check_clean();
   }
   init();
   device.KTCallbacks.pfnWaitForSynchronizationObjectFromGpuCb = NULL;
   CHECK(RuntimeDeviceCreateContext(&device) == DXGI_ERROR_UNSUPPORTED);
   check_clean();

   for (unsigned failure = 0; failure < 3; ++failure) {
      init();
      CHECK(RuntimeDeviceCreateContext(&device) == S_OK);
      fail_signal = failure == 0; fail_submit = failure == 1; fail_wait = failure == 2;
      CHECK(FAILED(RuntimeDeviceFlush(&device)));
      CHECK(signals == 1 && waits == (failure == 2 ? 1u : 0u) && !cpu_waits);
      CHECK(FAILED(RuntimeDeviceFlush(&device)) && flushes == 1);
      CHECK(RuntimeDeviceDestroyContext(&device) == S_OK);
      check_clean();
   }
   puts("PASS: actual D3D runtime timeline bridge, GPU callback ordering, CPU drain, handle ownership and failure cleanup; mocked APIs, no GPU execution");
   return 0;
}
