param([Parameter(Mandatory=$true)][string]$Directory)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$p=[IO.Path]::GetFullPath($Directory)
$id='ACPI\SWOA0F01\0'
function Device-Property([string]$name) {
    (Get-PnpDeviceProperty -InstanceId $id -KeyName ('DEVPKEY_Device_'+$name)).Data
}
function Check-File([string]$path,$expected) {
    $file=Get-Item -LiteralPath $path
    if ($file.Length -ne $expected.Length -or (Get-FileHash -LiteralPath $path).Hash -ne $expected.Sha256) {
        throw ('Artifact mismatch: '+$path)
    }
}
function Copy-Checked([string]$source,[string]$destination,$expected) {
    Check-File $source $expected
    Copy-Item -LiteralPath $source -Destination $destination -Force
    Check-File $destination $expected
}

if ((Device-Property 'DriverVersion') -ne '0.1.0.59' -or (Device-Property 'ProblemCode') -ne 0) {
    throw 'Expected .59 Code 0 before staging the .158 retest'
}
foreach ($name in 'icd-registration.json','automatic-start.json','native-result.json','before-acpitabl.dat') {
    if (Test-Path (Join-Path $p $name)) { throw ('Existing trial record: '+$name) }
}
$m=[IO.File]::ReadAllText((Join-Path $p 'trial-components.json'))|ConvertFrom-Json
if ($m.kmd_version -ne '0.1.0.158' -or
    $m.kmd.'tegragpurender.sys'.Sha256 -ne '46d1757674ff156028ce5b729ba53671981ffab09948fcaf125751c46bb10088' -or
    $m.app.'TegraGpuD3D.dll'.Sha256 -ne '02248b25419fb5560d0ffb17b3eb7d2b699ca4769a9ae08e0bfe8e6f0083cb13' -or
    $m.app.'test_d3d10_runtime_present.exe'.Sha256 -ne 'f1695ede879a5062ba144997be54a131ec29fcc25d1aa34a662de23d346e88a1' -or
    $m.app.'libvulkan_nouveau.dll'.Sha256 -ne 'ad6865784890f937a7c29aab0f108841b6d99cbffdca985b7dafbffc1a953625' -or
    $m.shared_probe -ne '--shared-layout-updates (event-query completion; no source-image readback)') {
    throw 'This entry point expects the identified .158/query-poll/timeline candidate components'
}
$app=Join-Path $p 'app'
foreach ($entry in $m.app.PSObject.Properties) {
    Check-File (Join-Path $app $entry.Name) $entry.Value
}
foreach ($entry in $m.scripts.PSObject.Properties) {
    if ((Get-FileHash -LiteralPath (Join-Path $p $entry.Name)).Hash -ne $entry.Value) {
        throw ('Script mismatch: '+$entry.Name)
    }
}
# Keep the frozen, signed KMD and known helper binaries. New UMD/ICD files
# come from this trial's app manifest, never a historical UMD control folder.
$package='C:\ProgramData\SwitchWOA\GpuUpdate\render-only-0.1.0.158'
$helpers='C:\ProgramData\SwitchWOA\GpuUpdate\stabilize158-native-20261112'
$copied=@()
foreach ($entry in $m.kmd.PSObject.Properties) {
    Copy-Checked (Join-Path $package $entry.Name) (Join-Path $p $entry.Name) $entry.Value
    $copied+=@{Name=$entry.Name;Length=$entry.Value.Length;Sha256=$entry.Value.Sha256}
}
foreach ($entry in $m.helpers.PSObject.Properties) {
    Copy-Checked (Join-Path $helpers $entry.Name) (Join-Path $p $entry.Name) $entry.Value
    $copied+=@{Name=$entry.Name;Length=$entry.Value.Length;Sha256=$entry.Value.Sha256}
}
$copied|ConvertTo-Json -Depth 5|Set-Content (Join-Path $p 'package-manifest.json') -Encoding UTF8
$appManifest=@{}
foreach ($file in Get-ChildItem $app -File) {
    if ($file.Name -ne 'app-manifest.json') {
        $appManifest[$file.Name]=@{Length=$file.Length;Sha256=(Get-FileHash $file.FullName).Hash}
    }
}
$appManifest|ConvertTo-Json -Depth 5|Set-Content (Join-Path $app 'app-manifest.json') -Encoding UTF8

# This executes the actual Zink surface functions with Vulkan substitutes.
# It neither executes GPU commands nor establishes native pixel correctness.
$exe=Join-Path $app 'test-zink-win32-surface.exe'
$child=[Diagnostics.Process]::new()
$child.StartInfo.FileName=$exe
$child.StartInfo.WorkingDirectory=$app
$child.StartInfo.UseShellExecute=$false
$child.StartInfo.CreateNoWindow=$true
$child.StartInfo.RedirectStandardOutput=$true
$child.StartInfo.RedirectStandardError=$true
$watch=[Diagnostics.Stopwatch]::StartNew()
try {
    $null=$child.Start()
    $stdout=$child.StandardOutput.ReadToEndAsync()
    $stderr=$child.StandardError.ReadToEndAsync()
    $finished=$child.WaitForExit(20000)
    if (!$finished) { $child.Kill();$child.WaitForExit() }
    $code=$child.ExitCode
    [IO.File]::WriteAllText((Join-Path $p 'surface-control.stdout'),$stdout.Result)
    [IO.File]::WriteAllText((Join-Path $p 'surface-control.stderr'),$stderr.Result)
} finally { $child.Dispose();$watch.Stop() }
$record=[ordered]@{
    Utc=[DateTime]::UtcNow.ToString('o')
    Exit=$code
    Timeout=(!$finished)
    Milliseconds=$watch.ElapsedMilliseconds
    Sha256=(Get-FileHash $exe).Hash
    DriverAfter=(Device-Property 'DriverVersion')
    ProblemAfter=(Device-Property 'ProblemCode')
    Scope='Actual Zink surface functions with Vulkan substitutes; no GPU execution'
}
$record|ConvertTo-Json|Set-Content (Join-Path $p 'surface-control-result.json') -Encoding UTF8
if (!$finished -or $code -ne 0 -or $record.DriverAfter -ne '0.1.0.59' -or $record.ProblemAfter -ne 0) {
    throw 'ARM64 surface control failed or the baseline changed'
}
# Check real loader discovery before installing/starting the native KMD.
# API substitutes and direct ICD loading cannot detect a SYSTEM task whose
# driver-path environment override is ignored by the Khronos loader.
& (Join-Path $PSScriptRoot 'Invoke-NvkCheck.ps1') -Directory $p -LoaderOnly
'STAGED: exact .158 KMD, query-poll UMD and timeline ICD; surface control and registered-ICD loader discovery passed; native rendering remains untested'
