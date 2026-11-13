# SPDX-License-Identifier: MIT
# The .158 miniport consumes a durable permit during StartDevice. Keep its
# grant across boots and rearm only after a successful native startup. This
# task never starts/restarts a device or switches a logged-in desktop.
param([ValidateSet('Configure','Rearm')][string]$Mode='Rearm')
. (Join-Path $PSScriptRoot 'Common.ps1')
Require-Administrator
$m=Check-Package
if($m.startup_mode -ne 'native-boot'){throw 'Expected a direct native boot package'}
$service='SYSTEM\CurrentControlSet\Services\tegragpurender'
$permit=Join-Path $env:windir 'System32\drivers\TgGpuBootTest158.bin'
$r=@{Utc=[DateTime]::UtcNow.ToString('o');Mode=$Mode;OsRebootRequested=$false;GpuRestartRequested=$false;PhysicalPanelVerified=$false}
$svc=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($service,$true)
try{
    if((Device-Property 'DriverVersion') -ne '0.1.0.158'){throw 'Unexpected installed kernel version'}
    Check-File "$env:windir\System32\drivers\tegragpurender.sys" $m.kmd.'tegragpurender.sys'
    Check-File "$env:windir\System32\acpitabl.dat" $m.helpers.'display158.aml'
    $registration=Read-Json (Join-Path $PSScriptRoot 'icd-registration.json')
    $key=[Microsoft.Win32.Registry]::LocalMachine.OpenSubKey($registration.Subkey)
    try{
        $slots=@($key.GetValue('UserModeDriverName'))
        if($slots.Count -ne 4 -or $slots[1] -ne $registration.TemporaryUmd -or
           $slots[2] -ne $slots[1] -or $key.GetValue('VulkanDriverName') -ne $registration.Manifest){throw 'Native UMD/ICD registration changed'}
    }finally{$key.Dispose()}
    if($Mode -eq 'Configure'){
        Check-Baseline
        $null=Sync-Recovery
        # Recreate rather than reopen a possibly volatile desktop grant.
        $svc.DeleteSubKey('AllowDesktopStart',$false)
        $key=$svc.CreateSubKey('AllowDesktopStart',[Microsoft.Win32.RegistryKeyPermissionCheck]::ReadWriteSubTree,[Microsoft.Win32.RegistryOptions]::None)
        try{$key.Flush()}finally{$key.Dispose()}
        $svc.Flush()
    }else{
        $limit=[DateTime]::UtcNow.AddSeconds(20)
        do{
            $problem=Device-Property 'ProblemCode'
            $g=Get-ItemProperty ('HKLM:\'+$service+'\Parameters')
            if($problem -eq 0 -and $g.HardwareStatus -eq 0 -and $g.PanelStatus -eq 0){break}
            if($problem -eq 43){throw 'Native boot failed; no device restart will be attempted'}
            Start-Sleep -Milliseconds 500
        }while([DateTime]::UtcNow -lt $limit)
        if($problem -ne 0 -or $g.HardwareStatus -ne 0 -or $g.PanelStatus -ne 0){throw 'Native startup did not finish'}
        # Retain the existing passive startup interval before allowing another
        # boot attempt. A reset during startup leaves the consumed permit closed.
        Start-Sleep -Seconds 30
        $problem=Device-Property 'ProblemCode'
        $g=Get-ItemProperty ('HKLM:\'+$service+'\Parameters')
        if($problem -ne 0 -or $g.HardwareStatus -ne 0 -or $g.PanelStatus -ne 0){throw 'Native startup did not remain active'}
        if(!(Test-Path ('HKLM:\'+$service+'\AllowDesktopStart'))){throw 'Native boot grant was removed'}
        $r.ProblemCode=$problem;$r.HardwareStatus=$g.HardwareStatus;$r.PanelStatus=$g.PanelStatus
    }
    $data=[byte[]]::new(4096);$data[0]=1
    $file=[IO.FileStream]::new($permit,[IO.FileMode]::Create,[IO.FileAccess]::Write,[IO.FileShare]::Read,4096,[IO.FileOptions]::WriteThrough)
    try{$file.Write($data,0,$data.Length);$file.Flush($true)}finally{$file.Dispose()}
    $r.Status=if($Mode -eq 'Configure'){'ArmedForNextBoot'}else{'NativeActiveNextBootArmed'}
    $r.Boot=(Get-CimInstance Win32_OperatingSystem).LastBootUpTime.ToUniversalTime().ToString('o')
    Save-Json (Join-Path $PSScriptRoot 'native-boot-state.json') $r
    $r|ConvertTo-Json
}catch{
    $svc.DeleteSubKey('AllowDesktopStart',$false);$svc.Flush()
    if(Test-Path $permit){Remove-Item $permit -Force}
    Disable-ScheduledTask Tgd3dDefaultEnable -ErrorAction SilentlyContinue|Out-Null
    $r.Status='Failed';$r.Error=$_.Exception.Message
    Save-Json (Join-Path $PSScriptRoot 'native-boot-state.json') $r
    throw
}finally{$svc.Dispose()}
