# SPDX-License-Identifier: MIT
$ErrorActionPreference='Stop'
$settings=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Services\tegragpurender\Tgd3dDefault'
$dir=[IO.Path]::GetFullPath([string]$settings.Directory)
$root=[IO.Path]::GetFullPath((Join-Path $env:ProgramData 'SwitchWOA\GpuUpdate')).TrimEnd('\')+'\'
if(!$dir.StartsWith($root,[StringComparison]::OrdinalIgnoreCase)){throw 'Installed directory is outside the protected package root'}
& (Join-Path $dir 'Restore-AfterBoot.ps1')
