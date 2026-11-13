# SPDX-License-Identifier: MIT
# Adapted from the retained .158/.161 rollback, with a trial-local .59 export.
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$dir=$PSScriptRoot
$task='Tgd3dTrialRestore'
Start-Transcript -Path (Join-Path $dir 'restore-transcript.log') -Append
try {
    # Once per trial, before the native driver/UMD registration is removed.
    # The writer has its own memory/time bounds and captures a PSS snapshot.
    try { & (Join-Path $dir 'Capture-PreRecovery.ps1') }
    catch { Write-Output ('Pre-recovery evidence unavailable: '+$_.Exception.Message) }
    foreach($name in 'Tgd3dTrialAutomatic','Tgd3dTrialObserve','Tgd3dTrialNative') {
        if(Get-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue) {
            Stop-ScheduledTask -TaskName $name -ErrorAction SilentlyContinue
            Unregister-ScheduledTask -TaskName $name -Confirm:$false
        }
    }
    $grant='HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\AllowDesktopStart'
    if(Test-Path $grant) { Remove-Item $grant -Force }
    $permit="$env:windir\System32\drivers\TgGpuBootTest158.bin"
    if(Test-Path $permit) {
        Copy-Item $permit (Join-Path $dir 'startup-contract.bin') -Force
        Remove-Item $permit -Force
    }
    try { & (Join-Path $dir 'Stop-DxgBootTrace.ps1') -TraceName 'Tgd3dTrialDxg' }
    catch { Write-Output ('Trace cleanup: '+$_.Exception.Message) }
    if(Get-EtwTraceSession -Name 'Tgd3dTrialUmd' -ErrorAction SilentlyContinue) {
        try { Stop-EtwTraceSession -Name 'Tgd3dTrialUmd' }
        catch { Write-Output ('UMD trace cleanup: '+$_.Exception.Message) }
    }
    foreach($name in 'umd-live-process.json','trace-flush-process.json') {
        $path=Join-Path $dir $name
        if(Test-Path $path) {
            $record=Read-Json $path
            Stop-RecordedTrialProcess $record
        }
    }
    # Candidate files need not still be intact to recover. Validate the backups.
    $backup=Check-Recovery
    $registration=Join-Path $dir 'icd-registration.json'
    if(Test-Path $registration) {
        $icd=Read-Json $registration
        $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey([string]$icd.Subkey,$true)
        if($key) {
            try {
                if($key.GetValue('VulkanDriverName') -eq [string]$icd.Manifest) { $key.DeleteValue('VulkanDriverName',$false) }
                if(@($key.GetValue('UserModeDriverName')) -contains [string]$icd.TemporaryUmd) {
                    $key.SetValue('UserModeDriverName',[string[]]$icd.PreviousUmd,[Microsoft.Win32.RegistryValueKind]::MultiString)
                }
            } finally { $key.Dispose() }
        }
    }
    $table="$env:windir\System32\acpitabl.dat"
    $baseline='B829B50944A90745FB6F6F7F44F453A6B8573FE4019A95FEFD0013A983703518'
    if((File-Hash $table) -eq '515D8D9C1612514CA2F197D6C96774A9265785A7E519921A195F0F8D257AA500') {
        $pending=Join-Path $dir 'restore-acpitabl.pending'
        Copy-Item (Join-Path $dir 'rollback\acpitabl.dat') $pending -Force
        [IO.File]::Replace($pending,$table,(Join-Path $dir 'test-acpitabl.saved'))
    }
    if((File-Hash $table) -ne $baseline) { throw 'Unexpected ACPI table; preserve it for manual inspection' }
    $packages=@(Get-WindowsDriver -Online|Where-Object {
        [IO.Path]::GetFileName($_.OriginalFileName) -eq 'tegragpurender.inf' -and $_.ProviderName -eq 'SwitchWOA' -and
        $_.Version -eq '0.1.0.158' -and $_.Driver -match '^oem[0-9]+\.inf$'
    })
    foreach($package in $packages) {
        & pnputil.exe /delete-driver $package.Driver /uninstall
        if($LASTEXITCODE -notin @(0,3010)) { throw ('Remove candidate failed: '+$LASTEXITCODE) }
    }
    $inf=Package-Path $backup.Inf
    if((Device-Property 'DriverVersion') -ne '0.1.0.59') {
        & pnputil.exe /add-driver $inf /install
        if($LASTEXITCODE -notin @(0,3010)) { throw ('Stage .59 failed: '+$LASTEXITCODE) }
        if((Device-Property 'DriverVersion') -ne '0.1.0.59') {
            # pnputil /install does not force a lower-ranked package.
            Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class Tgd3dRestoreDriver {
    [DllImport("newdev.dll", CharSet=CharSet.Unicode, SetLastError=true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    public static extern bool UpdateDriverForPlugAndPlayDevicesW(IntPtr parent, string hardwareId, string inf, uint flags, out bool reboot);
}
'@
            $reboot=$false
            if(![Tgd3dRestoreDriver]::UpdateDriverForPlugAndPlayDevicesW([IntPtr]::Zero,'ACPI\SWOA0F01',$inf,5,[ref]$reboot)) {
                throw ('Force .59 binding failed: '+[Runtime.InteropServices.Marshal]::GetLastWin32Error())
            }
        }
    }
    if((Device-Property 'DriverVersion') -ne '0.1.0.59') { throw 'Baseline driver did not bind' }
    if((File-Hash "$env:windir\System32\drivers\tegragpuprobe.sys") -ne 'DE5E9CD492D35A73EA3D3F5F1723FD82F88922F6E91EF5966B430360F5EB5646') { throw 'Restored .59 file differs' }
    $action=New-ScheduledTaskAction -Execute "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -Argument ('-NoProfile -ExecutionPolicy Bypass -File "'+$dir+'\restore-preference.ps1"')
    $trigger=New-ScheduledTaskTrigger -AtLogOn -User $backup.User;$trigger.Delay='PT20S'
    $principal=New-ScheduledTaskPrincipal -UserId $backup.User -LogonType Interactive -RunLevel Highest
    $settings=New-ScheduledTaskSettingsSet -AllowStartIfOnBatteries -DontStopIfGoingOnBatteries -ExecutionTimeLimit (New-TimeSpan -Minutes 1)
    Register-ScheduledTask -TaskName 'Tgd3dTrialPreference' -Action $action -Trigger $trigger -Principal $principal -Settings $settings -Force|Out-Null
    @{Utc=[DateTime]::UtcNow.ToString('o');Version=(Device-Property 'DriverVersion');Table=(File-Hash $table);RebootVerificationPending=$true}|
        ConvertTo-Json|Set-Content (Join-Path $dir 'restore-staged.json') -Encoding UTF8
    & shutdown.exe /r /t 4
    if($LASTEXITCODE -ne 0) { throw 'Recovery reboot request failed' }
    Unregister-ScheduledTask -TaskName $task -Confirm:$false -ErrorAction SilentlyContinue
} catch {
    $_|Out-String|Set-Content (Join-Path $dir 'restore-error.txt') -Encoding UTF8
    throw
} finally { Stop-Transcript }
