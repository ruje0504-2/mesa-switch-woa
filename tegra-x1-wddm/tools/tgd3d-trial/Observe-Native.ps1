# SPDX-License-Identifier: MIT
# Five-minute API/composition observation. Physical panel feedback is separate.
$ErrorActionPreference='Stop';$p=$PSScriptRoot;$e=Join-Path $p 'evidence'
if(!(Get-Process -Id $PID).SessionId) { throw 'Run on the interactive desktop' }
$result=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');ApiObservationPassed=$false;PhysicalPanelVerified=$false;Captures=@()}
$watch=[Diagnostics.Stopwatch]::StartNew()
try {
    $expected=$null
    foreach($second in @(0,30,60,90,120,150,180,210,240,270,300)) {
        while($watch.Elapsed.TotalSeconds -lt $second) { Start-Sleep -Milliseconds 200 }
        $prefix=Join-Path $e ('native-'+$second)
        & "$p\Assert-NativeDesktop.ps1" -Directory $p -OutputPath ($prefix+'-identity.json') -ExpectedLuid $expected
        $identity=Get-Content -Raw ($prefix+'-identity.json')|ConvertFrom-Json
        if(!$expected) { $expected=$identity.Luid }
        if(($second % 60) -eq 0) {
            $captureArgs='-NoProfile -ExecutionPolicy Bypass -File "'+$p+'\Capture-DesktopEvidence.ps1" -OutputPrefix "'+$prefix+'"'
            $child=Start-Process "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" -ArgumentList $captureArgs -WindowStyle Hidden -PassThru -RedirectStandardOutput ($prefix+'.log') -RedirectStandardError ($prefix+'.err')
            if(!$child.WaitForExit(15000)) { $child.Kill();throw 'Desktop capture timed out' }
            if($child.ExitCode -ne 0) { throw 'Desktop capture failed' }
            $result.Captures+=($prefix+'.json')
        }
    }
    $result.ApiObservationPassed=$true
} catch { $result.Error=$_.Exception.Message }
finally {
    $result.End=[DateTime]::UtcNow.ToString('o');$result.ElapsedSeconds=$watch.Elapsed.TotalSeconds
    $result|ConvertTo-Json -Depth 5|Set-Content "$e\native-observation.json" -Encoding UTF8
}
if(!$result.ApiObservationPassed) { exit 1 }
