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
    if((Device-Property 'DriverVersion') -ne '0.1.0.59' -or (Device-Property 'ProblemCode') -ne 0) { throw 'Expected the working .59 Code 0 baseline' }
    if((File-Hash "$env:windir\System32\drivers\tegragpuprobe.sys") -ne 'DE5E9CD492D35A73EA3D3F5F1723FD82F88922F6E91EF5966B430360F5EB5646') { throw 'Installed .59 driver differs' }
    if((File-Hash "$env:windir\System32\acpitabl.dat") -ne 'B829B50944A90745FB6F6F7F44F453A6B8573FE4019A95FEFD0013A983703518') { throw 'ACPI baseline differs' }
}
function Check-Recovery {
    $backup=Read-Json (Join-Path $script:TrialRoot 'recovery.json')
    if($backup.InstanceId -ne $script:DeviceId -or $backup.Version -ne '0.1.0.59') { throw 'Recovery identity differs' }
    foreach($entry in $backup.Files.PSObject.Properties) { Check-File (Package-Path $entry.Name) $entry.Value }
    $inf=Package-Path $backup.Inf
    if([IO.Path]::GetFileName($inf) -ne 'tegragpuprobe.inf') { throw 'Recovery INF differs' }
    if(!(Test-Path (Join-Path (Split-Path $inf) 'tegragpuprobe.cat'))) { throw 'Recovery catalogue missing' }
    $backup
}
