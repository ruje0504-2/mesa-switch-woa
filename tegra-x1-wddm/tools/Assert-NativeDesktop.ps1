# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [string]$ExpectedLuid
)

$ErrorActionPreference='Stop'
$p=[IO.Path]::GetFullPath($Directory)
if(Test-Path -LiteralPath $OutputPath){throw 'Native identity record already exists'}
$session=(Get-Process -Id $PID).SessionId
$r=[ordered]@{Utc=[DateTime]::UtcNow.ToString('o');Session=$session;Verified=$false;PhysicalPanelVerified=$false}
try {
    if(!$session){throw 'Run the identity check on the interactive desktop'}
    $id='ACPI\SWOA0F01\0'
    $r.Version=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
    $r.Problem=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
    $m=[IO.File]::ReadAllText((Join-Path $p 'trial-components.json'))|ConvertFrom-Json
    if($r.Version -ne $m.kmd_version -or $r.Problem -ne 0){throw 'The identified native KMD is not running with Code 0'}
    $helper=Join-Path $p 'desktop-path.exe'
    if((Get-FileHash $helper).Hash -ne $m.helpers.'desktop-path.exe'.Sha256){throw 'Display path helper differs from the trial manifest'}
    $rows=@(& $helper | ForEach-Object {$_|ConvertFrom-Json})
    if($LASTEXITCODE -ne 0 -or $rows.Count -ne 2){throw 'Active display path query failed'}
    $r.Paths=$rows
    $r.Luid=$rows[1].kmt_luid
    if(!$rows[0].active_path -or !$rows[1].matching_kmd -or !$rows[1].nvk_ready -or
        $rows[0].source_luid -ne $r.Luid -or $rows[0].target_luid -ne $r.Luid -or
        $r.Luid -eq '0000000000000000' -or ($ExpectedLuid -and $ExpectedLuid -ne $r.Luid)) {
        throw 'The active desktop is not on the expected native GM20B adapter'
    }
    $dwm=@(Get-Process dwm | Where-Object {$_.SessionId -eq $session})
    if($dwm.Count -ne 1){throw 'Expected one DWM process in the interactive session'}
    $r.DwmPid=$dwm[0].Id
    $r.DwmStart=$dwm[0].StartTime.ToUniversalTime().ToString('o')
    # The receiver is still writing. Retain the matching current-process line,
    # rather than using a previous boot's successful loader message.
    $f=[IO.FileStream]::new((Join-Path $p 'loader-debug.csv'),[IO.FileMode]::Open,[IO.FileAccess]::Read,([IO.FileShare]::ReadWrite -bor [IO.FileShare]::Delete))
    $reader=[IO.StreamReader]::new($f)
    try{$text=$reader.ReadToEnd()}finally{$reader.Dispose()}
    $messages=@($text -split '[\r\n]+' | Where-Object {$_} | ConvertFrom-Csv -Header Sequence,Clock,Pid,Message)
    $driver=Join-Path $p 'app\libvulkan_nouveau.dll'
    $loaded=@($messages|Where-Object {$_.Pid -eq [string]$r.DwmPid -and $_.Message.Contains('Using "NVIDIA Tegra X1 (GM20B)') -and $_.Message.Contains($driver)})
    if(!$loaded.Count){throw 'No current DWM loader record for the trial NVK ICD'}
    $r.DwmLoader=$loaded[-1]
    $r.Verified=$true
} catch {
    $r.Error=$_.Exception.Message
    throw
} finally {
    $r.End=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($OutputPath,($r|ConvertTo-Json -Depth 6),[Text.UTF8Encoding]::new($false))
}
