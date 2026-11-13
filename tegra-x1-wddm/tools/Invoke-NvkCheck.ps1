# SPDX-License-Identifier: MIT
param(
    [Parameter(Mandatory=$true)][string]$Directory,
    [switch]$LoaderOnly
)

$ErrorActionPreference='Stop'
$ProgressPreference='SilentlyContinue'
$directory=[IO.Path]::GetFullPath($Directory)
$app=Join-Path $directory 'app'
$name=if($LoaderOnly){'loader'}else{'gpu-timelines'}
$path=Join-Path $directory ($name+'-result.json')
if(Test-Path $path){throw ('Existing result: '+$path)}
$record=[ordered]@{Begin=[DateTime]::UtcNow.ToString('o');State='Started';Pass=$false;Timeout=$false;
    Scope=if($LoaderOnly){'Loader instance discovery on .59; no GPU commands'}else{'Native GPU memory/timeline handoffs; no DWM or physical screen acceptance'}}
$child=$null;$key=$null;$registered=$false
$registry='SOFTWARE\Khronos\Vulkan\Drivers'
$manifest=Join-Path $app 'nvk_windows_icd.json'
try {
    $identity=[Security.Principal.WindowsIdentity]::GetCurrent()
    $principal=[Security.Principal.WindowsPrincipal]::new($identity)
    $record.Identity=$identity.Name
    $record.Administrator=$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if(!$record.Administrator){throw 'Run this registration check from an elevated PowerShell'}
    $id='ACPI\SWOA0F01\0'
    $record.DriverBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
    $record.ProblemBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
    if($LoaderOnly -and ($record.DriverBefore -ne '0.1.0.59' -or $record.ProblemBefore -ne 0)){
        throw 'Run loader preflight on the .59 Code 0 baseline before arming a native trial'
    }
    $env:NVK_I_WANT_A_BROKEN_VULKAN_DRIVER='1'
    $exe=Join-Path $app $(if($LoaderOnly){'nvk-windows-check.exe'}else{'nvk-windows-vulkan-shared-check.exe'})
    $record.ExecutableSha256=(Get-FileHash $exe).Hash
    $record.LoaderSha256=(Get-FileHash (Join-Path $app 'vulkan-1.dll')).Hash
    $record.IcdSha256=(Get-FileHash (Join-Path $app 'libvulkan_nouveau.dll')).Hash
    $record.ManifestSha256=(Get-FileHash $manifest).Hash
    # Elevated processes ignore VK_DRIVER_FILES. Use the loader's normal
    # machine registry discovery, without changing any D3D UMD slot or any
    # other ICD registration. The native trial's recovery script must also
    # remove this exact value if Windows restarts before finally runs.
    $key=[Microsoft.Win32.Registry]::LocalMachine.CreateSubKey($registry)
    if($key.GetValueNames() -contains $manifest){throw 'This ICD manifest is already registered; preserve its owner'}
    $record.RegistrySubkey=$registry
    $record.RegistryValue=$manifest
    [IO.File]::WriteAllText($path,($record|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
    $key.SetValue($manifest,0,[Microsoft.Win32.RegistryValueKind]::DWord)
    $registered=$true
    $arguments=if($LoaderOnly){'--loader "'+$app+'\vulkan-1.dll"'}else{'"'+$app+'\vulkan-1.dll" --gpu-timelines'}
    $stdout=Join-Path $directory ($name+'-stdout.log')
    $stderr=Join-Path $directory ($name+'-stderr.log')
    # The native probe flushes each case. Preserve partial output on disk.
    $child=Start-Process -FilePath $exe -ArgumentList $arguments -WorkingDirectory $app -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $null=$child.Handle
    $done=$child.WaitForExit($(if($LoaderOnly){15000}else{120000}))
    if(!$done){$child.Kill();$child.WaitForExit()}
    $record.Exit=$child.ExitCode
    $record.Timeout=!$done
    $record.Output=[IO.File]::ReadAllText($stdout)
    $record.StandardError=[IO.File]::ReadAllText($stderr)
    $record.DriverAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
    $record.ProblemAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
    if($LoaderOnly){
        # check.c emits JSON only after vkCreateInstance succeeded. On .59,
        # the loader may return -3 for zero physical devices. Preserve that
        # raw result: successful discovery is not successful GPU execution.
        $data=$record.Output|ConvertFrom-Json
        $record.InstanceCreated=$data.loader -eq $true -and $data.native_ready_kmd -eq 0 -and
            $data.physical_devices -eq 0 -and $data.enumerate_result -in @(0,-3)
        $record.Pass=$done -and $record.Exit -in @(0,10) -and $record.InstanceCreated -and
            $record.DriverAfter -eq '0.1.0.59' -and $record.ProblemAfter -eq 0
    }else{
        $record.Pass=$done -and $record.Exit -eq 0 -and
            $record.Output.Contains('checked_words=49920; final host wait only; no presentation') -and
            $record.DriverBefore -eq $record.DriverAfter -and
            $record.ProblemBefore -eq 0 -and $record.ProblemAfter -eq 0
    }
} catch {
    $record.Error=($_|Out-String)
} finally {
    if($child){$child.Dispose()}
    try {
        if($registered){
            $key.DeleteValue($manifest,$false)
            $record.RegistrationRemoved=!($key.GetValueNames() -contains $manifest)
            if(!$record.RegistrationRemoved){$record.Pass=$false}
        }
    } catch {
        $record.Pass=$false
        $record.CleanupError=($_|Out-String)
    } finally {
        if($key){$key.Dispose()}
    }
    $record.State='Finished'
    $record.End=[DateTime]::UtcNow.ToString('o')
    [IO.File]::WriteAllText($path,($record|ConvertTo-Json -Depth 5),[Text.UTF8Encoding]::new($false))
}
$record|ConvertTo-Json -Depth 5
if(!$record.Pass){throw ('NVK '+$name+' check failed; see '+$path)}
