# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$TraceName
)
$ErrorActionPreference='Stop'
$p=[IO.Path]::GetFullPath($Directory)
$manifest=[IO.File]::ReadAllText((Join-Path $p 'trial-components.json'))|ConvertFrom-Json
$exe=Join-Path $p 'app\tgd3d-trace-export.exe'
if((Get-FileHash -LiteralPath $exe).Hash -ne $manifest.app.'tgd3d-trace-export.exe'.Sha256){throw 'UMD consumer differs from the trial manifest'}
foreach($name in 'umd-live.jsonl','umd-live.log','umd-live-process.json'){
    if(Test-Path (Join-Path $p $name)){throw 'UMD live consumer was already started for this trial'}
}
$consumer=$null
try{
    $consumer=Start-Process -FilePath $exe -ArgumentList @('--follow',('"'+$TraceName+'"')) -PassThru -WindowStyle Hidden -RedirectStandardOutput (Join-Path $p 'umd-live.jsonl') -RedirectStandardError (Join-Path $p 'umd-live.log')
    $limit=[Diagnostics.Stopwatch]::StartNew()
    $ready=$null
    do{
        if($consumer.HasExited){throw ('UMD live consumer exited: '+$consumer.ExitCode)}
        $f=[IO.FileStream]::new((Join-Path $p 'umd-live.jsonl'),[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::ReadWrite)
        $r=[IO.StreamReader]::new($f)
        try{$text=$r.ReadToEnd()}finally{$r.Dispose()}
        $end=$text.IndexOf("`n")
        if($end -ge 0){$ready=$text.Substring(0,$end)|ConvertFrom-Json;break}
        Start-Sleep -Milliseconds 50
    }while($limit.ElapsedMilliseconds -lt 4000)
    if(!$ready -or $ready.event -ne 'TraceStart' -or $ready.pid -ne $consumer.Id -or
        $ready.session -cne $TraceName -or $ready.clock -ne 'qpc' -or $ready.qpc_frequency -ne [Diagnostics.Stopwatch]::Frequency -or $ready.query_status -ne 0 -or $ready.invalid -ne 0 -or $consumer.HasExited){
        throw 'UMD live consumer did not attach before the native trial'
    }
    $record=[ordered]@{Pid=$consumer.Id;StartTime=$consumer.StartTime.ToUniversalTime().ToString('o');Exe=$exe;TraceName=$TraceName;Ready=$ready}
    [IO.File]::WriteAllText((Join-Path $p 'umd-live-process.json'),($record|ConvertTo-Json -Depth 4),[Text.UTF8Encoding]::new($false))
    [pscustomobject]$record
}catch{
    if($consumer -and !$consumer.HasExited){$consumer.Kill();$consumer.WaitForExit()}
    throw
}
