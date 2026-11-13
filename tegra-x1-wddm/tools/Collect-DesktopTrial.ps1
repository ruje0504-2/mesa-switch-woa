# SPDX-License-Identifier: MIT
param([Parameter(Mandatory=$true)][string]$Directory)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$p=[IO.Path]::GetFullPath($Directory)
if(!(Test-Path "$p\recovery-verified.json")){throw 'Recovery verification missing'}
if(Get-ScheduledTask -TaskName 'TgGpuDisplay*' -ErrorAction SilentlyContinue){throw 'Trial still active'}
if(Test-Path "$p\retained-records.zip"){throw 'Retained archive already exists; do not replace its manifest'}
$m=[IO.File]::ReadAllText("$p\trial-components.json")|ConvertFrom-Json
$exclude=@($m.kmd.PSObject.Properties.Name)+@($m.helpers.PSObject.Properties.Name)+@('retained-records.zip','retained-manifest.json','retained-archive.json')
$files=@(Get-ChildItem -LiteralPath $p -File|Where-Object {$_.Name -notin $exclude})
$manifest=@($files|ForEach-Object {
    # In interrupted trials, directory enumeration returned stale lengths for
    # loader-debug.csv and trace-flush.bin. Measure the same stream we hash.
    $f=[IO.File]::Open($_.FullName,[IO.FileMode]::Open,[IO.FileAccess]::Read,[IO.FileShare]::Read)
    $sha=[Security.Cryptography.SHA256]::Create()
    try {
        [pscustomobject]@{
            Name=$_.Name
            Length=$f.Length
            Sha256=([BitConverter]::ToString($sha.ComputeHash($f))).Replace('-','')
        }
    } finally {$sha.Dispose();$f.Dispose()}
})
[IO.File]::WriteAllText("$p\retained-manifest.json",($manifest|ConvertTo-Json -Depth 3),[Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip=[IO.Compression.ZipFile]::Open("$p\retained-records.zip",[IO.Compression.ZipArchiveMode]::Create)
try {
    foreach($f in $files){$null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($zip,$f.FullName,$f.Name,[IO.Compression.CompressionLevel]::Optimal)}
} finally {$zip.Dispose()}
$r=[ordered]@{
    Utc=[DateTime]::UtcNow.ToString('o')
    Count=$files.Count
    Bytes=($manifest|ForEach-Object {[long]$_.Length}|Measure-Object -Sum).Sum
    Measurement='Length and SHA256 from the same open file stream'
    ArchiveLength=(Get-Item "$p\retained-records.zip").Length
    ArchiveSha256=(Get-FileHash "$p\retained-records.zip").Hash
}
[IO.File]::WriteAllText("$p\retained-archive.json",($r|ConvertTo-Json),[Text.UTF8Encoding]::new($false))
$r|ConvertTo-Json
