# Desktop image evidence

Use [`Capture-DesktopEvidence.ps1`](../tools/Capture-DesktopEvidence.ps1) in
the logged-in interactive session to inspect taskbar position/content, Start
and tray visibility, and missing desktop regions. Inspect the returned PNG;
a visible `Shell_TrayWnd` rectangle or successful Present is not a pixel check.
Opening the Start menu remains a separate interaction check.

The script captures the actual primary-screen bounds in physical pixels,
without assuming portrait dimensions. Its adjacent JSON records UTC start/end,
session and input desktop, driver version/problem before and after, taskbar
owner/rectangle and PNG SHA256. Keep the trial's active display-path/LUID and
exact component hashes alongside these files: the installed driver version
alone does not identify the adapter that composed the image.

## Trial integration

Retain the important probe/trace records before attempting a screenshot. Run
the capture as a hidden child of the existing interactive trial controller,
with a unique prefix for each observation. For example, where `$captureScript`
and `$outputPrefix` are absolute paths chosen by that controller:

```powershell
$arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -OutputPrefix "{1}"' -f $captureScript, $outputPrefix
$capture = Start-Process "$env:windir\System32\WindowsPowerShell\v1.0\powershell.exe" `
    -ArgumentList $arguments -WindowStyle Hidden -PassThru
if (-not $capture.WaitForExit(30000)) {
    $capture.Kill()
    throw 'Desktop capture exceeded 30 seconds'
}
if ($capture.ExitCode -ne 0) { throw "Desktop capture failed: $($capture.ExitCode)" }
```

The surrounding trial must retain its automatic restoration path even if
capture fails or times out. Session 0 or a non-`Default` input desktop is
rejected; do not interpret that failure as a black rendered frame. Existing
output files are preserved rather than overwritten. Fetch the PNG and JSON,
check their device hashes and inspect the actual image during the native
interval before calling the desktop correct. Do not substitute a screenshot
taken after `.59` restoration for a native result.

A screenshot observes the Windows desktop capture path. It does not establish
physical scanout, panel rotation, absence of a physical black screen, or
sustained stability. Keep `PhysicalPanelVerified=false` and correlate with
scanout records and physical observation when validating those properties.

## Verified baseline control

`desktop-capture-control-20261111` captures the recovered `.59` Code 0 desktop
from 23:30:06 to 23:30:12 UTC. The driver remains `.59` Code 0 and the input
desktop remains `Default`. The image is 720x1280; the taskbar rectangle is
`(0,1232)-(720,1280)`. Direct inspection confirms the bottom taskbar, Start
button, Wi-Fi, volume and clock. A foreground command window covers part of
the upper desktop in this control; this is not a clean full-desktop comparison.
Future controllers should launch the child hidden as above.

PNG SHA256:
`7599cbaaf68ea1123126f1ed0e5e488ebeea53677685c008f48eef33029b5d21`.
All eight returned files match the device lengths/hashes, and the control
task is removed. This verifies capture on `.59`, not corrected native output.

The user also supplied a 720x1280 historical screenshot with wallpaper/icons
but no taskbar, Start button or tray, identifying it as the earlier `.146`
capture. Its SHA256 is
`06091e339cb3d5f1be55d2df91a2b3e64dce46ad87f73ba15098bfeca10133c4`.
The missing taskbar is directly visible. The supplied file has no embedded
driver identity and is not byte-identical to a retained run PNG, so its `.146`
attribution remains the user's identification. Wallpaper/icons alone must not
be described as a complete desktop pass or used as a known-good taskbar baseline.
