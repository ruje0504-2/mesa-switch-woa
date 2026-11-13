# SPDX-License-Identifier: MIT
# Execute the real evidence reader with controlled ETW JSONL, without a GPU.
$ErrorActionPreference='Stop'
$reader=Join-Path $PSScriptRoot '../tools/Read-NativeUmdEvidence.ps1'
$tmp=Join-Path ([IO.Path]::GetTempPath()) ([Guid]::NewGuid().ToString()+'.jsonl')
$now=[DateTime]::SpecifyKind([DateTime]'2026-11-13T04:00:00',[DateTimeKind]::Utc)
$begin=$now.AddSeconds(-20)
$clock=$now.ToFileTimeUtc()
$count=0
function Consumer($event,$offset=0){
    @{event=$event;time=$clock+$offset;pid=456;session='NativeControl';query_status=0;process_status=0;invalid=0;events_lost=2;log_buffers_lost=0;realtime_buffers_lost=3}
}
function Event($id,$event,$status,$offset){
    @{id=$id;event=$event;status=$status;time=$clock+$offset;pid=123;image='dwm.exe'}
}
function Run($name,$events,$accept,$tail=''){
    [IO.File]::WriteAllText($tmp,((@($events|ForEach-Object {$_|ConvertTo-Json -Compress}) -join "`n")+"`n"+$tail),[Text.UTF8Encoding]::new($false))
    $result=$null;$errorText=$null
    try{$result=& $reader -Path $tmp -DwmProcessId 123 -DwmStart $begin.AddSeconds(-10) -TrialStart $begin -TraceName NativeControl -Now $now}catch{$errorText=$_.Exception.Message}
    if($accept -and (!$result -or $errorText)){throw ($name+': expected acceptance, got '+$errorText)}
    if(!$accept -and !$errorText){throw ($name+': incorrectly accepted')}
    $script:count++
    Write-Output ('PASS '+$name)
}
try{
    $start=Consumer TraceStart -300000000
    $state=Consumer TraceState
    $create=Event 11 CreateDeviceResult '00000000' -150000000
    $present=Event 13 Present '00000000' -10000000
    Run 'current DWM post-call success' @($start,$create,$present,$state) $true
    Run 'loader/collector alone is insufficient' @($start,$state) $false
    Run 'creation without Present' @($start,$create,$state) $false
    Run 'Present without creation' @($start,$present,$state) $false
    Run 'failed hardware creation' @($start,(Event 11 CreateDeviceResult '80004005' -150000000),$present,$state) $false
    Run 'software NO_REDIRECTION creation' @($start,(Event 11 CreateDeviceResult '087a0004' -150000000),$present,$state) $false
    Run 'latest Present failed' @($start,$create,$present,(Event 13 Present '80004005' -5000000),$state) $false
    $other=$present.Clone();$other.pid=124
    Run 'different DWM process' @($start,$create,$other,$state) $false
    Run 'reused PID or old trial creation' @($start,(Event 11 CreateDeviceResult '00000000' -400000000),$present,$state) $false
    Run 'destruction after old success' @($start,$create,$present,(Event 27 DestroyDeviceBegin '00000000' -5000000),$state) $false
    Run 'new failed creation after old Present' @($start,$create,$present,(Event 11 CreateDeviceResult '80004005' -5000000),$state) $false
    Run 'new creation then Present' @($start,$create,(Event 27 DestroyDeviceBegin '00000000' -30000000),(Event 11 CreateDeviceResult '00000000' -20000000),$present,$state) $true
    Run 'collector exited' @($start,$create,$present,$state,(Consumer TraceEnd)) $false
    Run 'stale collector state' @($start,$create,$present,(Consumer TraceState -110000000)) $false
    $late=Consumer TraceStart -190000000
    Run 'collector attached after trial began' @($late,$create,$present,$state) $false
    foreach($field in 'events_lost','log_buffers_lost','realtime_buffers_lost','invalid','query_status'){
        $bad=$state.Clone();$bad[$field]++
        Run ('failed counter '+$field) @($start,$create,$present,$bad) $false
    }
    Run 'trailing incomplete JSON ignored' @($start,$create,$present,$state) $true '{"event":'
    Run 'success not yet covered by a complete buffer' @($start,$create,$state,$present) $false
    Run 'malformed complete record rejected' @($start,$create,$present,$state) $false "{broken}`n"
    Write-Output ("PASS: $count CPU evidence controls; no ETW session or GPU execution")
}finally{Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue}
