# Takes the focus from the Unity editor and gives it back, so that it refreshes its assets (and compiles changed scripts).
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class F {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetShellWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
}
"@
$u = Get-Process Unity -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
$other = Get-Process | Where-Object { $_.MainWindowHandle -ne 0 -and $_.Id -ne $u.Id -and $_.ProcessName -match 'explorer|Code|claude|Claude' } | Select-Object -First 1
[F]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [F]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
if ($other) { [void][F]::SetForegroundWindow($other.MainWindowHandle) } else { [void][F]::SetForegroundWindow([F]::GetShellWindow()) }
Start-Sleep -Milliseconds 800
[F]::keybd_event(0x12, 0, 0, [UIntPtr]::Zero); [F]::keybd_event(0x12, 0, 2, [UIntPtr]::Zero)
[void][F]::SetForegroundWindow($u.MainWindowHandle)
"bounced via $($other.ProcessName)"
