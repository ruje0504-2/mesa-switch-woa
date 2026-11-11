# SPDX-License-Identifier: MIT
# Run in the interactive session as a bounded child of the trial controller.
# Pair the PNG with the trial's active display-path/LUID records. A desktop
# capture is composition evidence, not proof of the physical panel's output.
param([Parameter(Mandatory=$true)][string]$OutputPrefix)
$ErrorActionPreference='Stop';$ProgressPreference='SilentlyContinue'
$started=[DateTime]::UtcNow.ToString('o')
if((Get-Process -Id $PID).SessionId -eq 0){throw 'Capture requires an interactive session'}
foreach($suffix in '.png','.json','.pending.png'){
 if(Test-Path -LiteralPath ($OutputPrefix+$suffix)){throw 'Capture output already exists'}
}
Add-Type @'
using System;
using System.Text;
using System.Runtime.InteropServices;
public static class TgDesktopCapture {
 [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern IntPtr OpenInputDesktop(uint flags,bool inherit,uint access);
 [DllImport("user32.dll")] public static extern bool CloseDesktop(IntPtr desktop);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool GetUserObjectInformation(IntPtr handle,int index,StringBuilder value,uint bytes,out uint needed);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string cls,string title);
 [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr window);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr window,out Rect rect);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr window,out uint process);
 public static string InputDesktop() {
  IntPtr desktop=OpenInputDesktop(0,false,1);
  if(desktop==IntPtr.Zero) throw new InvalidOperationException("Input desktop unavailable");
  try { var name=new StringBuilder(256);uint needed;
   if(!GetUserObjectInformation(desktop,2,name,512,out needed)) throw new InvalidOperationException("Desktop name unavailable");
   return name.ToString();
  } finally { CloseDesktop(desktop); }
 }
}
'@
$desktop=[TgDesktopCapture]::InputDesktop()
if($desktop -ne 'Default'){throw ('Capture requires Default desktop; current='+$desktop)}
$dpi=[TgDesktopCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))
if($dpi -eq [IntPtr]::Zero){throw 'Could not select physical pixel coordinates'}
$record=@{Begin=$started;Session=(Get-Process -Id $PID).SessionId;Desktop=$desktop;PhysicalPanelVerified=$false;Exit=1}
try{
 Add-Type -AssemblyName System.Drawing
 Add-Type -AssemblyName System.Windows.Forms
 $id='ACPI\SWOA0F01\0'
 $record.DriverBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
 $record.ProblemBefore=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
 $bounds=[Windows.Forms.Screen]::PrimaryScreen.Bounds
 $record.Bounds=@{X=$bounds.X;Y=$bounds.Y;Width=$bounds.Width;Height=$bounds.Height}
 $bar=[TgDesktopCapture]::FindWindow('Shell_TrayWnd',$null)
 $rect=New-Object TgDesktopCapture+Rect;[uint32]$owner=0
 $valid=[TgDesktopCapture]::GetWindowRect($bar,[ref]$rect)
 $null=[TgDesktopCapture]::GetWindowThreadProcessId($bar,[ref]$owner)
 $record.Taskbar=@{Found=$bar -ne [IntPtr]::Zero;Visible=[TgDesktopCapture]::IsWindowVisible($bar);Pid=$owner;BoundsValid=$valid;Bounds=$rect}
 $image=New-Object Drawing.Bitmap($bounds.Width,$bounds.Height)
 try{
  $g=[Drawing.Graphics]::FromImage($image)
  try{$g.CopyFromScreen($bounds.X,$bounds.Y,0,0,$image.Size)}finally{$g.Dispose()}
  $image.Save(($OutputPrefix+'.pending.png'),[Drawing.Imaging.ImageFormat]::Png)
 }finally{$image.Dispose()}
 Move-Item -LiteralPath ($OutputPrefix+'.pending.png') -Destination ($OutputPrefix+'.png')
 $record.DriverAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_DriverVersion).Data
 $record.ProblemAfter=(Get-PnpDeviceProperty -InstanceId $id -KeyName DEVPKEY_Device_ProblemCode).Data
 $record.DesktopAfter=[TgDesktopCapture]::InputDesktop()
 $record.ImageSha256=(Get-FileHash -LiteralPath ($OutputPrefix+'.png')).Hash
 $record.Exit=0
}catch{$record.Error=($_|Out-String)}finally{
 $record.End=[DateTime]::UtcNow.ToString('o')
 $null=[TgDesktopCapture]::SetThreadDpiAwarenessContext($dpi)
 $record|ConvertTo-Json -Depth 5|Set-Content -LiteralPath ($OutputPrefix+'.json') -Encoding UTF8
}
if($record.Exit){exit 1}
