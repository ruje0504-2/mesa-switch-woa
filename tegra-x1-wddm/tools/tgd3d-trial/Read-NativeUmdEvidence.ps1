# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Path,
    [Parameter(Mandatory=$true)][int]$DwmProcessId,
    [Parameter(Mandatory=$true)][DateTime]$DwmStart,
    [Parameter(Mandatory=$true)][DateTime]$TrialStart,
    [Parameter(Mandatory=$true)][string]$TraceName,
    [Parameter(Mandatory=$true)][string]$ExpectedLuid,
    [DateTime]$Now=[DateTime]::UtcNow
)
$ErrorActionPreference='Stop'

# The consumer appends JSONL while this check runs. Only complete lines count;
# a partial last line is neither a successful event nor a corrupt old record.
$file=[IO.FileStream]::new($Path,[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
$reader=[IO.StreamReader]::new($file)
try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
$end=$text.LastIndexOf("`n")
if($end -lt 0){throw 'No complete UMD trace records'}
$rows=@($text.Substring(0,$end) -split '[\r\n]+' | Where-Object {$_} | ForEach-Object {$_|ConvertFrom-Json})
$start=@($rows|Where-Object {$_.event -eq 'TraceStart'})
$state=@($rows|Where-Object {$_.event -eq 'TraceState'}|Select-Object -Last 1)
if($start.Count -ne 1 -or $state.Count -ne 1 -or @($rows|Where-Object {$_.event -eq 'TraceEnd'}).Count){
    throw 'UMD live consumer is not collecting complete buffers'
}
$start=$start[0];$state=$state[0]
$nowTime=$Now.ToUniversalTime().ToFileTimeUtc()
$trialTime=$TrialStart.ToUniversalTime().ToFileTimeUtc()
if($start.session -cne $TraceName -or $state.session -cne $TraceName -or $state.pid -ne $start.pid -or
    $start.query_status -ne 0 -or $state.query_status -ne 0 -or $start.invalid -ne 0 -or $state.invalid -ne 0 -or
    $start.time -gt $trialTime -or $state.time -lt $trialTime -or $state.time -gt $nowTime -or
    $nowTime - $state.time -gt 100000000){
    throw 'UMD consumer identity, coverage or current trace state is invalid'
}
foreach($field in 'events_lost','log_buffers_lost','realtime_buffers_lost'){
    if($null -eq $start.$field -or $null -eq $state.$field -or $state.$field -ne $start.$field){
        throw ('UMD trace loss after consumer attachment: '+$field)
    }
}
$begin=[Math]::Max($trialTime,$DwmStart.ToUniversalTime().ToFileTimeUtc())
# TraceState follows a complete buffer. Do not accept a newer partial buffer
# whose validation/loss counters have not yet been published.
$stateIndex=$rows.Count-1
while($rows[$stateIndex].event -ne 'TraceState'){$stateIndex--}
$rows=$rows[0..$stateIndex]
$events=@($rows|Where-Object {$_.pid -eq $DwmProcessId -and $_.image -eq 'dwm.exe' -and
    $_.time -ge $begin -and $_.time -le $state.time}|Sort-Object time)
$created=@($events|Where-Object {$_.id -eq 4 -and $_.status -eq '00000000'}|Select-Object -Last 1)
$present=@($events|Where-Object {$_.id -eq 30 -and $_.kind -eq 2}|Select-Object -Last 1)
if(!$created.Count -or !$present.Count -or $present[0].status -ne '00000000' -or $present[0].time -lt $created[0].time){
    throw 'No successful hardware device creation and subsequent Present from current DWM'
}
# A previous device's Present cannot validate a later creation attempt or
# destruction in the same DWM process. A later successful Present is needed.
$lifecycle=@($events|Where-Object {$_.id -in @(4,5)}|Select-Object -Last 1)
if($lifecycle.Count -and $lifecycle[0].time -gt $present[0].time){
    throw 'DWM device lifecycle changed after its last successful Present'
}
$vulkan=@($events|Where-Object {$_.id -eq 3 -and $_.status -eq '00000000' -and $_.time -le $created[0].time}|Select-Object -Last 1)
if(!$vulkan.Count -or ('{0:x16}' -f [uint64]$vulkan[0].arg0) -ne $ExpectedLuid -or
    ([uint64]$vulkan[0].arg1 -shr 32) -ne 1) { throw 'No matching NVK LUID from current DWM' }
[pscustomobject]@{Vulkan=$vulkan[0];CreateDevice=$created[0];Present=$present[0];ConsumerStart=$start;ConsumerState=$state}
