# SPDX-License-Identifier: MIT
# Windows/admin integration control: actual ETW and exporter, synthetic CPU
# events marked control.exe. Never evidence of DWM or GPU execution.
param([Parameter(Mandatory=$true)][string]$Directory)
$ErrorActionPreference='Stop'
$p=[IO.Path]::GetFullPath($Directory)
$name='TgUmdControl-'+[Guid]::NewGuid().ToString('N')
$consumer=$null
$started=$false
$provider=$false
Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class UmdTraceControl {
    [StructLayout(LayoutKind.Sequential)]
    struct Descriptor { public ushort id; public byte version,channel,level,opcode; public ushort task; public ulong keyword; }
    [StructLayout(LayoutKind.Sequential, CharSet=CharSet.Unicode)]
    struct Payload { public int result; public uint reserved; public ulong arg0,arg1; [MarshalAs(UnmanagedType.ByValTStr,SizeConst=32)] public string image; }
    [StructLayout(LayoutKind.Sequential)]
    struct Data { public ulong pointer; public uint size,reserved; }
    [DllImport("advapi32.dll")] static extern uint EventRegister(ref Guid id,IntPtr callback,IntPtr context,out ulong handle);
    [DllImport("advapi32.dll")] static extern uint EventUnregister(ulong handle);
    [DllImport("advapi32.dll")] static extern uint EventWrite(ulong handle,ref Descriptor descriptor,uint count,ref Data data);
    [DllImport("advapi32.dll")] [return:MarshalAs(UnmanagedType.U1)] static extern bool EventEnabled(ulong handle,ref Descriptor descriptor);
    static ulong handle;
    public static void Register() {
        var guid=new Guid("78edcf0e-b51c-4b24-9b28-c86d48f521a9");
        uint error=EventRegister(ref guid,IntPtr.Zero,IntPtr.Zero,out handle);
        if(error!=0) throw new Exception("EventRegister="+error);
    }
    public static void Emit(ushort id,int result) {
        var descriptor=new Descriptor { id=id,version=1,level=4,keyword=1 };
        if(!EventEnabled(handle,ref descriptor)) throw new Exception("Control provider not enabled");
        var payload=new Payload { result=result,image="control.exe" };
        int size=Marshal.SizeOf(payload);
        if(size!=88) throw new Exception("Control payload layout differs");
        IntPtr memory=Marshal.AllocHGlobal(size);
        try {
            Marshal.StructureToPtr(payload,memory,false);
            var data=new Data { pointer=(ulong)memory.ToInt64(),size=(uint)size };
            uint error=EventWrite(handle,ref descriptor,1,ref data);
            if(error!=0) throw new Exception("EventWrite="+error);
        } finally { Marshal.FreeHGlobal(memory); }
    }
    public static void Unregister() { EventUnregister(handle); }
}
'@
try{
    New-EtwTraceSession -Name $name -LocalFilePath (Join-Path $p 'control.etl') -LogFileMode 0x101 -FlushTimer 1|Out-Null
    $started=$true
    Add-EtwTraceProvider -SessionName $name -Guid '{78edcf0e-b51c-4b24-9b28-c86d48f521a9}' -Level 4 -MatchAnyKeyword 1|Out-Null
    $consumer=& (Join-Path $PSScriptRoot '../tools/Start-NativeUmdTrace.ps1') -Directory $p -TraceName $name
    [UmdTraceControl]::Register();$provider=$true
    [UmdTraceControl]::Emit(11,0)
    # Three seconds of low-volume events exercise complete-buffer status
    # publication and the stdout reader without busy-polling the GPU.
    for($i=0;$i -lt 4;$i++){
        [UmdTraceControl]::Emit(13,0)
        Start-Sleep -Seconds 1
    }
    Stop-EtwTraceSession -Name $name|Out-Null;$started=$false
    $process=Get-Process -Id $consumer.Pid -ErrorAction SilentlyContinue
    if($process -and !$process.WaitForExit(8000)){throw 'Live consumer did not exit when its session stopped'}
    $rows=@(Get-Content (Join-Path $p 'umd-live.jsonl')|ForEach-Object {$_|ConvertFrom-Json})
    $create=@($rows|Where-Object {$_.id -eq 11 -and $_.image -eq 'control.exe' -and $_.status -eq '00000000' -and $_.pid -eq $PID})
    $presents=@($rows|Where-Object {$_.id -eq 13 -and $_.image -eq 'control.exe' -and $_.status -eq '00000000' -and $_.pid -eq $PID})
    $states=@($rows|Where-Object {$_.event -eq 'TraceState'})
    $last=$rows[-1]
    if($create.Count -ne 1 -or $presents.Count -ne 4 -or !$states.Count -or
        $last.event -ne 'TraceEnd' -or $last.process_status -ne 0 -or $last.invalid -ne 0){throw 'Live ETW control record counts/status differ'}
    $reference=$rows[0]
    foreach($state in $states){
        if($state.query_status -ne 0 -or $state.invalid -ne 0){throw 'Live ETW query/decoder failed'}
        foreach($field in 'events_lost','log_buffers_lost','realtime_buffers_lost'){
            if($state.$field -ne $reference.$field){throw ('Live ETW control lost records: '+$field)}
        }
    }
    if($create[0].time -lt $reference.time -or $create[0].time -gt [DateTime]::UtcNow.ToFileTimeUtc()){throw 'ETW timestamps are not FILETIME'}
    [ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Exit=0;CreateEvents=$create.Count;PresentEvents=$presents.Count;StateEvents=$states.Count;Collector=$consumer;Scope='Real ETW with synthetic CPU events; no native DWM or GPU rendering'}|ConvertTo-Json -Depth 5
}finally{
    if($provider){[UmdTraceControl]::Unregister()}
    if($started){Stop-EtwTraceSession -Name $name -ErrorAction SilentlyContinue|Out-Null}
    if($consumer){
        $process=Get-Process -Id $consumer.Pid -ErrorAction SilentlyContinue
        if($process -and $process.StartTime.ToUniversalTime() -eq [DateTime]::Parse($consumer.StartTime).ToUniversalTime() -and !$process.WaitForExit(4000)){$process.Kill()}
    }
}
