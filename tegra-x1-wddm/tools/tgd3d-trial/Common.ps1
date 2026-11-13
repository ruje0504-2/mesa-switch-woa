# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$script:TrialRoot=$PSScriptRoot
$script:DeviceId='ACPI\SWOA0F01\0'
function Require-Administrator {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    if(!([Security.Principal.WindowsPrincipal]::new($identity)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Run this entry point from an elevated administrator shell'
    }
}
function Read-Json([string]$Path) { [IO.File]::ReadAllText($Path)|ConvertFrom-Json }
function File-Hash([string]$Path) { (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash }
function Stop-RecordedTrialProcess($Record) {
    $process=Get-Process -Id $Record.Pid -ErrorAction SilentlyContinue
    if(!$process){return}
    try {
        # Retain the process object while checking identity; a recycled PID
        # must never select an unrelated process for termination.
        $null=$process.Handle
        if($process.StartTime.ToUniversalTime() -eq [DateTime]::Parse($Record.StartTime).ToUniversalTime() -and
           $process.ProcessName -eq [IO.Path]::GetFileNameWithoutExtension($Record.Exe)) {
            $process.Kill()
            if(!$process.WaitForExit(5000)){throw 'Owned collector did not exit'}
        }
    } catch {
        # ETW stop can make the consumer exit between lookup and Kill.
        if(!$process.HasExited){throw}
    } finally { $process.Dispose() }
}
function Device-Property([string]$Name) {
    (Get-PnpDeviceProperty -InstanceId $script:DeviceId -KeyName ('DEVPKEY_Device_'+$Name)).Data
}
function Package-Path([string]$Name) {
    $path=[IO.Path]::GetFullPath((Join-Path $script:TrialRoot $Name))
    if(!$path.StartsWith($script:TrialRoot.TrimEnd('\')+'\',[StringComparison]::OrdinalIgnoreCase)) { throw 'Path escapes the trial directory' }
    $path
}
function Check-File([string]$Path,$Expected) {
    $file=Get-Item -LiteralPath $Path
    if($file.Length -ne $Expected.Length -or (File-Hash $Path) -ne $Expected.Sha256) { throw ('File mismatch: '+$Path) }
}
function Check-Package {
    $m=Read-Json (Join-Path $script:TrialRoot 'trial-components.json')
    if($m.kmd_version -ne '0.1.0.158' -or $m.umd_name -ne 'TegraGpuD3DVk.dll') { throw 'Unexpected trial component identity' }
    foreach($entry in $m.files.PSObject.Properties) { Check-File (Package-Path $entry.Name) $entry.Value }
    foreach($file in Get-ChildItem -LiteralPath $script:TrialRoot -Filter '*.ps1') {
        $tokens=$null;$errors=$null
        $null=[System.Management.Automation.Language.Parser]::ParseFile($file.FullName,[ref]$tokens,[ref]$errors)
        if($errors.Count) { throw ('PowerShell parse failed: '+$file.Name) }
    }
    $m
}
function Check-Baseline {
    $version=Device-Property 'DriverVersion';$problem=Device-Property 'ProblemCode'
    if($version -eq '0.1.0.158' -and $problem -eq 43) {
        # Expected closed startup gate: BasicDisplay remains usable while the
        # retained .158 miniport is installed but native GPU work is disabled.
        if((Device-Property 'Service') -ne 'tegragpurender' -or
           (File-Hash "$env:windir\System32\drivers\tegragpurender.sys") -ne '46D1757674FF156028CE5B729BA53671981FFAB09948FCAF125751C46BB10088' -or
           (File-Hash "$env:windir\System32\acpitabl.dat") -ne '515D8D9C1612514CA2F197D6C96774A9265785A7E519921A195F0F8D257AA500') { throw 'Retained .158 baseline differs' }
    } elseif($version -eq '0.1.0.59' -and $problem -eq 0) {
        # One-time migration from the previous baseline; recovery never
        # automatically reinstalls this version.
        if((File-Hash "$env:windir\System32\drivers\tegragpuprobe.sys") -ne 'DE5E9CD492D35A73EA3D3F5F1723FD82F88922F6E91EF5966B430360F5EB5646' -or
           (File-Hash "$env:windir\System32\acpitabl.dat") -ne 'B829B50944A90745FB6F6F7F44F453A6B8573FE4019A95FEFD0013A983703518') { throw 'Initial .59 baseline differs' }
    } else { throw 'Expected the gated .158 baseline or initial .59 Code 0' }
}
function Check-Recovery {
    $backup=Read-Json (Join-Path $script:TrialRoot 'recovery.json')
    if($backup.InstanceId -ne $script:DeviceId -or $backup.Version -notin @('0.1.0.59','0.1.0.158')) { throw 'Recovery identity differs' }
    foreach($entry in $backup.Files.PSObject.Properties) { Check-File (Package-Path $entry.Name) $entry.Value }
    $inf=Package-Path $backup.Inf
    $name=if($backup.Version -eq '0.1.0.158'){'tegragpurender'}else{'tegragpuprobe'}
    if([IO.Path]::GetFileName($inf) -ne ($name+'.inf')) { throw 'Recovery INF differs' }
    if(!(Test-Path (Join-Path (Split-Path $inf) ($name+'.cat')))) { throw 'Recovery catalogue missing' }
    $backup
}
