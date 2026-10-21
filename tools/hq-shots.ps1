# Started by build-spm.ps1 when autotest\hq.txt exists: waits for SuperPaperMario.exe, then saves the GAME
# WINDOW's contents (PrintWindow, so other windows on top do not matter) at the seconds listed in hq.txt.
$auto = Join-Path (Split-Path -Parent $PSScriptRoot) 'autotest'
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System; using System.Runtime.InteropServices;
public static class W {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint flags);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
'@
[W]::SetProcessDPIAware() | Out-Null
$times = @((Get-Content (Join-Path $auto 'hq.txt') -Raw) -split '[,\s]+' | Where-Object { $_ } | ForEach-Object { [int]$_ })
Get-ChildItem $auto -Filter 'hq_*.png' | Remove-Item -Force -ErrorAction SilentlyContinue
$deadline = (Get-Date).AddMinutes(10)
while (-not ($p = Get-Process SuperPaperMario -ErrorAction SilentlyContinue)) { if ((Get-Date) -gt $deadline) { exit }; Start-Sleep -Milliseconds 200 }
$start = Get-Date
foreach ($t in ($times | Sort-Object)) {
    while (((Get-Date) - $start).TotalSeconds -lt $t) { Start-Sleep -Milliseconds 100 }
    $p = Get-Process SuperPaperMario -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $p) { break }
    $h = $p.MainWindowHandle; if ($h -eq 0) { continue }
    $r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T; if ($w -le 0 -or $hh -le 0) { continue }
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp); $dc = $g.GetHdc()
    [W]::PrintWindow($h, $dc, 3) | Out-Null   # PW_CLIENTONLY | PW_RENDERFULLCONTENT
    $g.ReleaseHdc($dc)
    $small = New-Object System.Drawing.Bitmap $bmp, ([int]($w / 3)), ([int]($hh / 3))
    $small.Save((Join-Path $auto ("hq_{0:D3}.png" -f $t)), [System.Drawing.Imaging.ImageFormat]::Png)
    $g.Dispose(); $bmp.Dispose(); $small.Dispose()
}
