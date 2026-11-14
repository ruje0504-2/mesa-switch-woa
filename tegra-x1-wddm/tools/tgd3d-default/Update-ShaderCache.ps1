# SPDX-License-Identifier: MIT
# Stage a cache-only UMD beside the active package. Activation is on reboot;
# never replace a mapped DLL or restart a running GPU device here.
[CmdletBinding()]
param([Parameter(Mandatory=$true)][string]$CandidateDirectory,[switch]$Restart)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$payload=[IO.File]::ReadAllText((Join-Path $CandidateDirectory 'update.json'))|ConvertFrom-Json
$settings='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
$previous=[string](Get-ItemProperty $settings).Directory
if($previous -ne $payload.expected_directory){throw 'Active package changed; inspect before updating'}
. (Join-Path $previous 'Common.ps1')
Require-Administrator
$m=Check-Package
$recovery=Check-Recovery
if($m.startup_mode -ne 'logon-default' -or $m.sync_mode -ne 'gpu-bridge' -or
   $payload.sync_mode -ne 'gpu-bridge' -or $payload.gpu_prerotation -or !$payload.cache.enabled){throw 'Expected a cache-only update of the logon GPU bridge package'}
if((Device-Property 'ProblemCode') -ne 0 -or
   (File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -ne $payload.expected_kmd -or
   (File-Hash ($previous+'\app\TegraGpuD3DVk.dll')) -ne $payload.expected_umd -or
   (File-Hash ($previous+'\app\libvulkan_nouveau.dll')) -ne $payload.expected_icd){throw 'Running component baseline differs'}
Check-File ($CandidateDirectory+'\TegraGpuD3DVk.dll') $payload.candidate_umd
Check-File ($CandidateDirectory+'\tgd3d-contracts.exe') $payload.control
$control=Read-Json ($CandidateDirectory+'\controls-state.json')
if($control.Phase -ne 'Passed' -or $control.Exit -ne 0 -or $control.Candidate -ne $payload.candidate_umd.Sha256 -or $control.GPUExecuted){throw 'Native CPU cache controls have not passed for this UMD'}
$task=Get-ScheduledTask Tgd3dDefaultEnable
if($task.State -ne 'Ready'){throw 'Native startup task must be idle and enabled'}
$state=Join-Path $CandidateDirectory 'installed.json'
if(Test-Path $state){throw 'Update already staged; inspect installed.json instead of repeating it'}
$dest=Join-Path (Split-Path $previous) ('tgd3d-cache-'+$payload.candidate_umd.Sha256.Substring(0,8)+'-'+(Get-Date -Format 'yyyyMMdd-HHmmss'))
if(Test-Path $dest){throw 'Destination already exists'}
New-Item -ItemType Directory $dest|Out-Null
# Retain the existing package and its exact recovery files. No KMD/ICD install.
$names=@($m.files.PSObject.Properties.Name)+@($recovery.Files.PSObject.Properties.Name)+@('recovery.json')
foreach($name in ($names|Select-Object -Unique)){
    $source=Package-Path $name;$target=Join-Path $dest $name
    New-Item -ItemType Directory (Split-Path $target) -Force|Out-Null
    Copy-Item -LiteralPath $source -Destination $target
}
Copy-Item ($CandidateDirectory+'\TegraGpuD3DVk.dll') ($dest+'\app\TegraGpuD3DVk.dll') -Force
Copy-Item ($CandidateDirectory+'\tgd3d-contracts.exe') ($dest+'\app\tgd3d-contracts.exe') -Force
Copy-Item ($PSScriptRoot+'\Restore-ShaderCache.ps1') ($dest+'\Restore-ShaderCache.ps1') -Force
foreach($name in @('TegraGpuD3DVk.dll','tgd3d-contracts.exe')){
    $file=Get-Item ($dest+'\app\'+$name);$record=@{Length=$file.Length;Sha256=(File-Hash $file.FullName)}
    $m.app.PSObject.Properties[$name].Value=$record
    $m.files.PSObject.Properties['app/'+$name].Value=$record
}
$restore=Get-Item ($dest+'\Restore-ShaderCache.ps1')
$m.files|Add-Member -NotePropertyName 'Restore-ShaderCache.ps1' -NotePropertyValue @{Length=$restore.Length;Sha256=(File-Hash $restore.FullName)} -Force
$m|Add-Member -NotePropertyName shader_cache -NotePropertyValue $payload.cache -Force
$m|Add-Member -NotePropertyName umd_frozen_base -NotePropertyValue $payload.frozen_base -Force
$m|Add-Member -NotePropertyName umd_patch_commits -NotePropertyValue $payload.patches -Force
$m.scope='.158 existing KMD/ICD with cache-only tgd3d UMD; logon start; cache performance acceptance pending'
Save-Json ($dest+'\trial-components.json') $m
$registration=Read-Json ($previous+'\icd-registration.json')
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey,$true)
try{
    [string[]]$old=@($key.GetValue('UserModeDriverName'));$oldIcd=[string]$key.GetValue('VulkanDriverName')
    if($old.Count -ne 4 -or $old[1] -ne ($previous+'\app\TegraGpuD3DVk.dll') -or $old[2] -ne $old[1]){throw 'Active UMD registration differs'}
    Export-ScheduledTask Tgd3dDefaultEnable|Set-Content ($dest+'\cache-prior-task.xml') -Encoding Unicode
    Save-Json ($dest+'\cache-update-backup.json') @{PreviousDirectory=$previous;Directory=$dest;Subkey=$registration.Subkey;UmdSlots=$old;Icd=$oldIcd;Kernel=$payload.expected_kmd;PreviousUmd=$payload.expected_umd;PreviousIcd=$payload.expected_icd;Utc=[DateTime]::UtcNow.ToString('o')}
    $registration.TemporaryUmd=$dest+'\app\TegraGpuD3DVk.dll';$registration.Manifest=$dest+'\app\nvk_windows_icd.json'
    Save-Json ($dest+'\icd-registration.json') $registration
    & icacls.exe $dest /inheritance:r /grant:r '*S-1-5-18:(OI)(CI)(F)' '*S-1-5-32-544:(OI)(CI)(F)' '*S-1-5-32-545:(OI)(CI)(RX)' /Q|Out-Null
    if($LASTEXITCODE){throw 'Package ACL failed'}
    & (Join-Path $dest 'Enable-RuntimeAppContainerRead.ps1') -Directory ($dest+'\app')|Set-Content ($dest+'\runtime-access.json') -Encoding UTF8
    . (Join-Path $dest 'Common.ps1')
    $null=Check-Package;$null=Sync-Recovery
    # Changes are reversible using the retained registration/task snapshot.
    [string[]]$updated=$old.Clone();$updated[1]=$updated[2]=$registration.TemporaryUmd
    $key.SetValue('UserModeDriverName',$updated,[Microsoft.Win32.RegistryValueKind]::MultiString)
    $key.SetValue('VulkanDriverName',$registration.Manifest,[Microsoft.Win32.RegistryValueKind]::String);$key.Flush()
    Set-ItemProperty $settings Directory $dest
    $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "'+$dest+'\Default-Start.ps1"')
    Set-ScheduledTask Tgd3dDefaultEnable -Action $action|Out-Null
    $r=@{Utc=[DateTime]::UtcNow.ToString('o');Directory=$dest;PreviousDirectory=$previous;Phase='StagedForReboot';Umd=$payload.candidate_umd.Sha256;Kernel=$payload.expected_kmd;Icd=$payload.expected_icd;KernelChanged=$false;IcdChanged=$false;GPUStartRequested=$false;OsRebootRequested=[bool]$Restart}
    Save-Json $state $r;$r|ConvertTo-Json
}catch{
    if(Test-Path ($dest+'\cache-update-backup.json')){& ($dest+'\Restore-ShaderCache.ps1')}
    throw
}finally{$key.Dispose()}
if($Restart){& shutdown.exe /r /t 10;if($LASTEXITCODE){throw 'Planned restart was not accepted'}}
