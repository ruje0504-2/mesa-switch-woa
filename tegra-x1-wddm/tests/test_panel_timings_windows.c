/* SPDX-License-Identifier: MIT
 * Real WDK layout, controlled display visibility; no hardware execution.
 */
#include "../windows/adapter.h"
#include <ntimage.h>

__declspec(dllimport) void __stdcall ExitProcess(ULONG code);
#define CHECK(x) do { if (!(x)) ExitProcess(__LINE__); } while (0)
static ULONG_PTR cookie = 0x2b992ddfa232ULL;
const IMAGE_LOAD_CONFIG_DIRECTORY64 _load_config_used = {
    .Size = sizeof(IMAGE_LOAD_CONFIG_DIRECTORY64),
    .SecurityCookie = (ULONGLONG)&cookie,
};

const D3DKMDT_VIDEO_SIGNAL_INFO TgGpuPanelSignal = {0};
static NTSTATUS visibility_status;
static ULONG visibility_calls;
static BOOLEAN visible;

NTSTATUS TgGpuPanelSetVisibility(TG_GPU_ADAPTER *adapter, BOOLEAN enabled)
{
    CHECK(adapter && adapter->Panel.Dc.owned);
    ++visibility_calls;
    if (NT_SUCCESS(visibility_status)) visible = enabled;
    return visibility_status;
}

static KIRQL current_irql(VOID) { return PASSIVE_LEVEL; }
KIRQL (*__imp_KeGetCurrentIrql)(VOID) = current_irql;

void *memset(void *dest, int value, size_t bytes)
{
    unsigned char *d = dest;
    for (size_t i = 0; i < bytes; ++i) d[i] = (unsigned char)value;
    return dest;
}

void *memcpy(void *dest, const void *source, size_t bytes)
{
    unsigned char *d = dest;
    const unsigned char *s = source;
    for (size_t i = 0; i < bytes; ++i) d[i] = s[i];
    return dest;
}

static ULONGLONG check_result(const DXGK_SET_TIMING_PATH_INFO *path,
    ULONGLONG previous, UINT target, UINT input)
{
    /* A zero ID or status makes Windows replace a successful timing with
     * LinkConfigurationFailed. Preserve all input and reserved fields. */
    CHECK(path->TargetState.ConnectionChangeId > previous);
    CHECK(path->TargetState.ConnectionStatus == 14);
    CHECK(path->TargetState.TargetId == target && !path->TargetState.Reserved);
    CHECK(path->VidPnTargetId == target && path->InputFlags == input);
    CHECK(!path->OutputFlags && path->DiagnosticInfo == 0x00ffffff);
    return path->TargetState.ConnectionChangeId;
}

void start(void)
{
    static TG_GPU_ADAPTER adapter;
    DRIVER_INITIALIZATION_DATA d = {0};
    TgGpuRegisterPanelVidPn(&d);
    adapter.Panel.Dc.owned = true;
    adapter.Panel.Boot.TargetId = 7;
    DXGK_SET_TIMING_PATH_INFO path = {.VidPnTargetId = 7,
        .Input = {.Active = 1, .PreserveInherited = 1, .VidPnPathUpdates = 1}};
    DXGK_SET_TIMING_RESULTS results = {.Value = UINT32_MAX};
    DXGKARG_SETTIMINGSFROMVIDPN args = {
        .pResultsFlags = &results, .PathCount = 1, .pSetTimingPathInfo = &path,
    };
    UINT input = path.InputFlags;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    CHECK(visible && visibility_calls == 1 && !results.Value);
    ULONGLONG id = check_result(&path, 0, 7, input);

    path.Input.Active = 0;
    path.Input.VidPnPathUpdates = 3; /* Remove the path, keep the panel connected. */
    input = path.InputFlags;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    CHECK(!visible && visibility_calls == 2);
    id = check_result(&path, id, 7, input);

    visibility_status = STATUS_IO_DEVICE_ERROR;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == visibility_status);
    CHECK(path.TargetState.ConnectionChangeId == id && visibility_calls == 3);
    visibility_status = STATUS_SUCCESS;

    path.VidPnTargetId = 8;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_GRAPHICS_INVALID_VIDEO_PRESENT_TARGET);
    CHECK(visibility_calls == 3 && path.TargetState.ConnectionChangeId == id);
    adapter.Panel.Boot.TargetId = 8;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS);
    (void)check_result(&path, id, 8, input);

    args.PathCount = 0;
    args.pSetTimingPathInfo = NULL;
    results.Value = UINT32_MAX;
    CHECK(d.DxgkDdiSetTimingsFromVidPn(&adapter, &args) == STATUS_SUCCESS && !results.Value);
    CHECK(visibility_calls == 4);
    ExitProcess(0);
}
