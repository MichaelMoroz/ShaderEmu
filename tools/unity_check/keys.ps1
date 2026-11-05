# Sends real keys and mouse buttons to the Unity editor when SeCheckProbe asks (se_check_keys.txt in the project).
# SPEC: tokens apart by spaces: hold:KEY:S (KEY a letter, Space, Up, Down, Left, Right, Ctrl, Esc, Enter), tap:KEY,
# lhold:S, rhold:S (mouse buttons), wait:S, wheel:N, move:DX:DY (relative mouse)
#   pwsh tools\unity_check\keys.ps1 -Project <the Unity project> [-Log FILE]     (beside the editor, until se_check_keys.quit)
param([Parameter(Mandatory = $true)][string]$Project, [string]$Log = '')
Add-Type @"
using System;
using System.Runtime.InteropServices;
public static class K {
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern void mouse_event(uint flags, int dx, int dy, int data, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  public static void Key(byte vk, bool up) {
    byte sc = (byte)MapVirtualKey(vk, 0);
    uint f = up ? 2u : 0u;
    if (vk >= 0x21 && vk <= 0x28) f |= 1u;   // extended: arrows and the like
    keybd_event(vk, sc, f, UIntPtr.Zero);
  }
}
"@
$vk = @{ Space = 0x20; Up = 0x26; Down = 0x28; Left = 0x25; Right = 0x27; Ctrl = 0xA2; Esc = 0x1B; Enter = 0x0D; End = 0x23; Home = 0x24; Insert = 0x2D; Alt = 0x12 }
function VK($k) { if ($vk.ContainsKey($k)) { return [byte]$vk[$k] } ; return [byte][char]$k.ToUpper() }
function Say($s) { if ($Log) { Add-Content $Log ("{0:HH:mm:ss.fff} {1}" -f (Get-Date), $s) } }
$req = Join-Path $Project 'se_check_keys.txt'
while ($true) {
    if (Test-Path $req) {
        $spec = (Get-Content $req -Raw).Trim()
        $u = Get-Process Unity -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
        if ($u) {
            [K]::Key(0x12, $false); [K]::Key(0x12, $true)   # (an Alt lets a background process bring a window to the front)
            [void][K]::SetForegroundWindow($u.MainWindowHandle)
            Start-Sleep -Milliseconds 300
            $front = [K]::GetForegroundWindow() -eq $u.MainWindowHandle
            Say "spec $spec (Unity in front: $front)"
            foreach ($t in $spec -split ' ') {
                $p = $t -split ':'
                switch ($p[0]) {
                    'hold' { $c = VK $p[1]; [K]::Key($c, $false); Start-Sleep -Milliseconds ([int]([double]$p[2] * 1000)); [K]::Key($c, $true) }
                    'tap' { $c = VK $p[1]; [K]::Key($c, $false); Start-Sleep -Milliseconds 80; [K]::Key($c, $true) }
                    'lhold' { [K]::mouse_event(2, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]([double]$p[1] * 1000)); [K]::mouse_event(4, 0, 0, 0, [UIntPtr]::Zero) }
                    'rhold' { [K]::mouse_event(8, 0, 0, 0, [UIntPtr]::Zero); Start-Sleep -Milliseconds ([int]([double]$p[1] * 1000)); [K]::mouse_event(16, 0, 0, 0, [UIntPtr]::Zero) }
                    'wheel' { [K]::mouse_event(0x800, 0, 0, [int]$p[1] * 120, [UIntPtr]::Zero) }
                    'move' { [K]::mouse_event(1, [int]$p[1], [int]$p[2], 0, [UIntPtr]::Zero) }
                    'wait' { Start-Sleep -Milliseconds ([int]([double]$p[1] * 1000)) }
                }
            }
        } else { Say "no Unity window for $spec" }
        Remove-Item $req -ErrorAction SilentlyContinue
    }
    if (Test-Path (Join-Path $Project 'se_check_keys.quit')) { Remove-Item (Join-Path $Project 'se_check_keys.quit'); break }
    Start-Sleep -Milliseconds 200
}
