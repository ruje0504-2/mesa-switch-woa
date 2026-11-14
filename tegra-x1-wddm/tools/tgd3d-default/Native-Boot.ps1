# SPDX-License-Identifier: MIT
# Configure the KMD's explicit persistent native-start mode. No PnP restart,
# startup task, permit file or user logon is needed for subsequent boots.
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
if($m.startup_mode -ne 'native-boot' -or $m.kmd_variant -notin @('shader-output-bit14-native-boot','shader-output-bit14-gpu460')){
    throw 'Direct boot requires the kernel with persistent native-start support'
}
# This only grants startup on a later boot; it does not hot-switch a running
# adapter. An already active, matching .158 may be armed after desktop testing.
if((Device-Property 'ProblemCode') -eq 0){
    if((Device-Property 'DriverVersion') -ne '0.1.0.158' -or (Device-Property 'Service') -ne 'tegragpurender'){throw 'Active native adapter differs'}
}else{Check-Baseline}
Check-File "$env:windir\System32\drivers\tegragpurender.sys" $m.kmd.'tegragpurender.sys'
$null=Sync-Recovery
$registration=Read-Json (Join-Path $PSScriptRoot 'icd-registration.json')
$key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey)
try{
    $slots=@($key.GetValue('UserModeDriverName'))
    if($slots.Count -ne 4 -or $slots[1] -ne $registration.TemporaryUmd -or
       $slots[2] -ne $slots[1] -or $key.GetValue('VulkanDriverName') -ne $registration.Manifest){throw 'Native UMD/ICD registration changed'}
}finally{$key.Dispose()}
$task=Get-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue
if($task){
    if($task.State -eq 'Running'){throw 'Logon activation is still running'}
    Disable-ScheduledTask Tgd3dDefaultEnable|Out-Null
}
$svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey('SYSTEM\CurrentControlSet\Services\tegragpurender',$true)
try{
    $svc.DeleteSubKey('AllowDesktopStart',$false)
    $permit=Join-Path $env:windir 'System32\drivers\TgGpuBootTest158.bin'
    if(Test-Path $permit){Remove-Item $permit -Force}
    $key=$svc.CreateSubKey('AllowNativeBoot',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Microsoft.Win32.RegistryOptions]::None)
    try{$key.Flush()}finally{$key.Dispose()}
    $svc.Flush()
}finally{$svc.Dispose()}
$r=@{Utc=[DateTime]::UtcNow.ToString('o');Status='PersistentNativeBootConfigured';OsRebootRequested=$false;GpuRestartRequested=$false;PhysicalPanelVerified=$false;StartupTaskRequired=$false;KernelHash=$m.kmd.'tegragpurender.sys'.Sha256}
Save-Json (Join-Path $PSScriptRoot 'native-boot-state.json') $r
$r|ConvertTo-Json
