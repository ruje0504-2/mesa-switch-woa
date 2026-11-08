/* SPDX-License-Identifier: MIT */
#include "gr_firmware_io.h"
#include "../core/execute.h"
#define FW_TAG 'wfGT'
static NTSTATUS ReadFirmware(PCWSTR path, ULONG maximum, struct tg_gpu_gr_blob *blob)
{
    UNICODE_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    FILE_STANDARD_INFORMATION info;
    HANDLE handle = NULL;
    PVOID data = NULL;
    NTSTATUS status;
    ULONG bytes;
    *blob = (struct tg_gpu_gr_blob){0};
    RtlInitUnicodeString(&name,path);
    InitializeObjectAttributes(&attributes,&name,OBJ_CASE_INSENSITIVE|OBJ_KERNEL_HANDLE,NULL,NULL);
    status = ZwCreateFile(&handle,FILE_READ_DATA|FILE_READ_ATTRIBUTES|SYNCHRONIZE,
        &attributes,&iosb,NULL,FILE_ATTRIBUTE_NORMAL,FILE_SHARE_READ,FILE_OPEN,
        FILE_SYNCHRONOUS_IO_NONALERT|FILE_NON_DIRECTORY_FILE,NULL,0);
    if (!NT_SUCCESS(status)) return status;
    status = ZwQueryInformationFile(handle,&iosb,&info,sizeof(info),FileStandardInformation);
    if (!NT_SUCCESS(status)) goto done;
    if (info.EndOfFile.QuadPart <= 0 || info.EndOfFile.QuadPart > maximum) {
        status=STATUS_INVALID_IMAGE_FORMAT; goto done;
    }
    bytes=(ULONG)info.EndOfFile.QuadPart;
    data=ExAllocatePool2(POOL_FLAG_PAGED,bytes,FW_TAG);
    if (!data) { status=STATUS_INSUFFICIENT_RESOURCES; goto done; }
    status=ZwReadFile(handle,NULL,NULL,NULL,&iosb,data,bytes,NULL,NULL);
    if (NT_SUCCESS(status) && iosb.Information != bytes) status=STATUS_END_OF_FILE;
    if (NT_SUCCESS(status)) { blob->data=data; blob->size=bytes; data=NULL; }
done:
    if (data) ExFreePoolWithTag(data,FW_TAG);
    ZwClose(handle);
    return status;
}
VOID TgGpuFirmwareFree(TG_GPU_FIRMWARE *firmware)
{
    if (firmware->surface) ExFreePoolWithTag(firmware->surface,FW_TAG);
    if (firmware->netlist) ExFreePoolWithTag(firmware->netlist,FW_TAG);
    RtlZeroMemory(firmware,sizeof(*firmware));
}
NTSTATUS TgGpuFirmwareLoad(TG_GPU_FIRMWARE *firmware)
{
    static PCWSTR const names[] = {
        L"\\SystemRoot\\System32\\drivers\\TegraGpu\\NETB_img.bin",
        L"\\SystemRoot\\System32\\drivers\\TegraGpu\\fecs.bin",
        L"\\SystemRoot\\System32\\drivers\\TegraGpu\\gpccs.bin"
    };
    struct tg_gpu_gr_blob files[3] = {0};
    struct tg_gpu_gr_payload *p=&firmware->payload;
    NTSTATUS status=STATUS_SUCCESS;
    RtlZeroMemory(firmware,sizeof(*firmware));
    /* Fixed driver-owned paths, read before powering up. No caller path,
     * physical address or firmware bytes are accepted by the IOCTL.
     */
    for (unsigned i=0;i<3;++i) {
        status=ReadFirmware(names[i],i ? 0x4000 : 0x100000,&files[i]);
        if (!NT_SUCCESS(status)) goto done;
    }
    if (tg_gpu_gr_netlist_parse(files[0],&p->netlist)!=TG_GPU_GR_FW_OK ||
        tg_gpu_gr_image_prepare(files[0],files+1,NULL,0,&p->image)!=TG_GPU_GR_FW_OK ||
        p->image.size>TG_GPU_GR_SURFACE_BYTES) {
        status=STATUS_INVALID_IMAGE_FORMAT; goto done;
    }
    firmware->surface=ExAllocatePool2(POOL_FLAG_PAGED,p->image.size,FW_TAG);
    if (!firmware->surface) { status=STATUS_INSUFFICIENT_RESOURCES; goto done; }
    if (tg_gpu_gr_image_prepare(files[0],files+1,firmware->surface,p->image.size,&p->image)
        !=TG_GPU_GR_FW_OK) { status=STATUS_INVALID_IMAGE_FORMAT; goto done; }
    p->surface=firmware->surface;
    firmware->netlist=(PVOID)files[0].data; files[0].data=NULL;
done:
    for (unsigned i=0;i<3;++i) if (files[i].data) ExFreePoolWithTag((PVOID)files[i].data,FW_TAG);
    if (!NT_SUCCESS(status)) TgGpuFirmwareFree(firmware);
    return status;
}
