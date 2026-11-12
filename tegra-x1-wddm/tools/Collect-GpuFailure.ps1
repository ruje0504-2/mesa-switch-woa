# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$OutputPath,
    [Parameter(Mandatory=$true)][datetime]$SinceUtc
)

# Read existing failure evidence. This does not enable/disable TDR, change
# dump/pagefile/power settings, start GPU work, or copy memory-dump contents.
$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$path=[IO.Path]::GetFullPath($OutputPath)
if(Test-Path -LiteralPath $path){throw 'Failure record already exists'}

function Property([string]$id,[string]$name) {
    (Get-PnpDeviceProperty -InstanceId $id -KeyName ('DEVPKEY_Device_'+$name) -ErrorAction SilentlyContinue).Data
}
function File-Identity([string]$name) {
    if(Test-Path -LiteralPath $name -PathType Leaf){
        $f=Get-Item -LiteralPath $name -Force
        @{Path=$f.FullName;Length=$f.Length;Sha256=(Get-FileHash -LiteralPath $f.FullName).Hash}
    }
}
function Device-Identity([string]$id) {
    $service=Property $id 'Service'
    $image=if($service){(Get-ItemProperty ('HKLM:\SYSTEM\CurrentControlSet\Services\'+$service) -ErrorAction SilentlyContinue).ImagePath}
    if($image){$image=[Environment]::ExpandEnvironmentVariables($image) -replace '^\\SystemRoot',$env:windir -replace '^\\\?\?\\',''}
    if($image -and $image -match '^System32\\'){$image=Join-Path $env:windir $image}
    @{Instance=$id;Version=(Property $id 'DriverVersion');Problem=(Property $id 'ProblemCode');
      Service=$service;Image=$image;File=$(if($image){File-Identity $image})}
}

$r=[ordered]@{
    BeginUtc=[DateTime]::UtcNow.ToString('o')
    SinceUtc=$SinceUtc.ToUniversalTime().ToString('o')
    CollectorSha256=(Get-FileHash -LiteralPath $PSCommandPath).Hash
    Scope='Existing system events, dump metadata and current configuration; not a native rendering test or a diagnosis'
}
$r.BootUtc=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
$r.CurrentDisplay=Device-Identity 'ACPI\SWOA0F01\0'
$r.SdHost=Device-Identity 'ACPI\NVDA0101\0'
try {
    $r.SystemEvents=@(Get-WinEvent -FilterHashtable @{
        LogName='System';StartTime=$SinceUtc.ToLocalTime();Id=41,46,161,1001,4101,6008
    } | Sort-Object RecordId | ForEach-Object {
        @{RecordId=$_.RecordId;Id=$_.Id;Provider=$_.ProviderName;
          Utc=$_.TimeCreated.ToUniversalTime().ToString('o');Message=$_.Message;Xml=$_.ToXml()}
    })
} catch {
    if($_.FullyQualifiedErrorId -notlike 'NoMatchingEventsFound*'){throw}
    $r.SystemEvents=@()
}
$r.CrashControl=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\CrashControl' |
    Select-Object CrashDumpEnabled,DumpFile,MinidumpDir,LogEvent,AutoReboot,Overwrite,DedicatedDumpFile,DumpFilters
$r.PageFileConfig=(Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\Session Manager\Memory Management').PagingFiles
$r.PageFiles=@(Get-CimInstance Win32_PageFileUsage | Select-Object Name,AllocatedBaseSize,CurrentUsage)
$r.TdrOverrides=Get-ItemProperty 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers' |
    Select-Object TdrLevel,TdrDelay,TdrDdiDelay,TdrDebugMode
$r.Dumps=@(Get-ChildItem 'C:\Windows\Minidump','C:\Windows\LiveKernelReports' -File -Recurse -ErrorAction SilentlyContinue |
    Select-Object FullName,Length,@{n='LastWriteUtc';e={$_.LastWriteTimeUtc.ToString('o')}})
$r.FullDump=Get-Item 'C:\Windows\MEMORY.DMP' -ErrorAction SilentlyContinue |
    Select-Object FullName,Length,@{n='LastWriteUtc';e={$_.LastWriteTimeUtc.ToString('o')}}
$r.EndUtc=[DateTime]::UtcNow.ToString('o')
[IO.File]::WriteAllText($path,($r|ConvertTo-Json -Depth 7),[Text.UTF8Encoding]::new($false))
File-Identity $path | ConvertTo-Json -Compress
