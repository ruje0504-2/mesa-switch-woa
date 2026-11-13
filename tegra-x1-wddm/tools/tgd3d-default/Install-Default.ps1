# SPDX-License-Identifier: MIT
param([switch]$Prepared)
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
if($m.startup_mode -ne 'logon-default'){throw 'Use a default-start package'}
if((Device-Property 'DriverVersion') -ne '0.1.0.158'){throw 'This package requires the retained .158 driver'}
if(!$Prepared){
    Check-Baseline
    $permit="$env:windir\System32\drivers\TgGpuBootTest158.bin"
    if(Test-Path $permit){
        $data=[IO.File]::ReadAllBytes($permit)
        if($data.Length -ne 4096 -or [BitConverter]::ToUInt32($data,0) -ne 0){throw 'A native start is already armed'}
        Remove-Item $permit
    }
    $output=@(& (Join-Path $PSScriptRoot 'Prepare.ps1'))
    $output|Write-Output
    $line=@($output|Where-Object {$_ -like 'PREPARED_DIRECTORY=*'})
    if($line.Count -ne 1){throw 'Preparation did not publish its installed directory'}
    $dest=$line[0].Substring('PREPARED_DIRECTORY='.Length)
    & (Join-Path $dest 'Install-Default.ps1') -Prepared
    return
}
$backup=Check-Recovery
Check-Baseline
$subkey='SYSTEM\CurrentControlSet\Control\Class\'+(Device-Property 'Driver')
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($subkey,$true)
try{
    [string[]]$old=@($key.GetValue('UserModeDriverName'))
    if($old.Count -ne 4 -or $old[0] -notlike '*\tegragpuno3d.dll' -or $old[3] -ne $old[0]){throw 'Unexpected original UMD slots'}
    Check-File $old[0] $m.kmd.'tegragpuno3d.dll'
    [string[]]$restore=@($old[0],$old[0],$old[0],$old[0])
    $umd=Join-Path $PSScriptRoot 'app\TegraGpuD3DVk.dll'
    $icd=Join-Path $PSScriptRoot 'app\nvk_windows_icd.json'
    @{Subkey=$subkey;Manifest=$icd;PreviousUmd=$restore;OriginalUmd=$old;TemporaryUmd=$umd;Utc=[DateTime]::UtcNow.ToString('o')}|
        ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'icd-registration.json') -Encoding UTF8
    $old[1]=$old[2]=$umd
    $key.SetValue('UserModeDriverName',$old,[Microsoft.Win32.RegistryValueKind]::MultiString)
    $key.SetValue('VulkanDriverName',$icd,[Microsoft.Win32.RegistryValueKind]::String)
}finally{$key.Dispose()}
$settings='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
New-Item $settings -Force|Out-Null
New-ItemProperty $settings Directory -PropertyType String -Value $PSScriptRoot -Force|Out-Null
$action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$PSScriptRoot+'\Default-Start.ps1"')
$trigger=New-ScheduledTaskTrigger -AtLogOn -User $backup.User
$trigger.Delay='PT5S'
$taskSettings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -MultipleInstances IgnoreNew -ExecutionTimeLimit (New-TimeSpan -Minutes 3)
Register-ScheduledTask -TaskName Tgd3dDefaultEnable -Action $action -Trigger $trigger -Settings $taskSettings -User SYSTEM -RunLevel Highest|Out-Null
$result=@{Utc=[DateTime]::UtcNow.ToString('o');Directory=$PSScriptRoot;Version=(Device-Property 'DriverVersion');SyncMode=$m.sync_mode;User=$backup.User;EnabledAfterNextLogon=$true;GpuStartRequested=$false;OsRebootRequested=$false}
$result|ConvertTo-Json|Set-Content (Join-Path $PSScriptRoot 'default-installed.json') -Encoding UTF8
$result|ConvertTo-Json
'INSTALLED: restart Windows when ready; tgd3d will enable after logon.'
