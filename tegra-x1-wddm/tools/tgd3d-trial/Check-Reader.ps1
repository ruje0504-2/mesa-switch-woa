# SPDX-License-Identifier: MIT
# CPU-only controls for current-process tgd3d evidence; emits no ETW or GPU work.
$ErrorActionPreference='Stop'
$dir=Join-Path ([IO.Path]::GetTempPath()) ('tgd3d-reader-'+[Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory $dir|Out-Null
$path=Join-Path $dir 'events.jsonl'
$now=[DateTime]::UtcNow;$trial=$now.AddSeconds(-3);$base=$trial.ToFileTimeUtc()
$readerArgs=@{Path=$path;DwmProcessId=77;DwmStart=$now.AddMinutes(-1);TrialStart=$trial;TraceName='test';ExpectedLuid='00000000000bc22f';Now=$now}
function Rows {
    @(
        @{event='TraceStart';time=$base-10000000;pid=22;session='test';query_status=0;invalid=0;events_lost=0;log_buffers_lost=0;realtime_buffers_lost=0},
        @{id=3;time=$base+1;pid=77;image='dwm.exe';status='00000000';arg0=770607;arg1=[uint64]4299161600},
        @{id=4;time=$base+2;pid=77;image='dwm.exe';status='00000000'},
        @{id=30;kind=2;time=$base+3;pid=77;image='dwm.exe';status='00000000'},
        @{event='TraceState';time=$base+20000000;pid=22;session='test';query_status=0;invalid=0;events_lost=0;log_buffers_lost=0;realtime_buffers_lost=0}
    )
}
function Check([string]$Name,$Rows,[bool]$Expected,[string]$Tail='') {
    $text=(@($Rows|ForEach-Object {$_|ConvertTo-Json -Compress}) -join "`n")+"`n"+$Tail
    [IO.File]::WriteAllText($path,$text)
    $ok=$false
    try { $null=& (Join-Path $PSScriptRoot 'Read-NativeUmdEvidence.ps1') @readerArgs;$ok=$true } catch { $message=$_.Exception.Message }
    if($ok -ne $Expected) { throw ('Reader control failed: '+$Name+' '+$message) }
    'PASS '+$Name
}
try {
    Check 'current tgd3d creation and Present' (Rows) $true
    Check 'partial trailing record ignored' (Rows) $true '{"id":30'
    $r=Rows;$r[1].arg0=1;Check 'wrong NVK LUID rejected' $r $false
    $r=Rows;$r[1].arg1=4194304;Check 'non-NVK rejected' $r $false
    $r=Rows;$r[2].status='80004005';Check 'failed creation rejected' $r $false
    $r=Rows;$r[3].status='887a0005';Check 'failed Present rejected' $r $false
    $r=Rows;$r[3].kind=1;Check 'Submit is not Present' $r $false
    $r=Rows;$r[3].pid=78;Check 'other process rejected' $r $false
    $r=Rows;$r[4].events_lost=1;Check 'lost events rejected' $r $false
    $r=Rows;$r[2].id=11;$r[3].id=13;Check 'Zink IDs rejected' $r $false
    $r=Rows;$r=$r[0..3]+@(@{id=5;time=$base+4;pid=77;image='dwm.exe';status='00000000'})+@($r[4]);Check 'destroy after Present rejected' $r $false
    $r=Rows;$r[4].time=$now.AddSeconds(-20).ToFileTimeUtc();Check 'stale state rejected' $r $false
} finally { Remove-Item $dir -Recurse -Force }
