/* SPDX-License-Identifier: MIT */
#include "diagnostic_events.h"

static const GUID provider_id={0xc27c1f9b,0x62e8,0x4e21,{0xb2,0x9b,0x6b,0xb2,0xd5,0x85,0xc6,0xd0}};
static REGHANDLE provider;

NTSTATUS TgGpuTraceInitialize(VOID)
{
    PAGED_CODE();
    return EtwRegister(&provider_id,NULL,NULL,&provider);
}
VOID TgGpuTraceShutdown(VOID)
{
    PAGED_CODE();
    if(provider){(VOID)EtwUnregister(provider);provider=0;}
}
VOID TgGpuTraceRecovery(const TG_GPU_RECOVERY_EVENT *record)
{
    const EVENT_DESCRIPTOR event={.Id=1,.Version=1,.Level=2,.Keyword=1};
    EVENT_DATA_DESCRIPTOR data={0};
    if(!provider)return;
    data.Ptr=(ULONGLONG)(ULONG_PTR)record;
    data.Size=sizeof(*record);
    /* This reports the first internal failure before recovery notification.
     * ETW loss/flush limits still apply; no disk I/O or GPU work is added. */
    (VOID)EtwWrite(provider,&event,NULL,1,&data);
}
