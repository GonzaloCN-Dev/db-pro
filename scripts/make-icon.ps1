# Draws the Db Pro icon (original artwork) at several sizes and writes assets/icon.png + assets/icon.ico.
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
$assets = Join-Path $root "assets"
New-Item -ItemType Directory -Force $assets | Out-Null

function New-IconBitmap([int]$size) {
    $bmp = New-Object System.Drawing.Bitmap $size, $size
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::Transparent)
    $r = [int]($size * 0.22)
    $rect = New-Object System.Drawing.Rectangle 0, 0, ($size - 1), ($size - 1)
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = $r * 2
    $path.AddArc($rect.X, $rect.Y, $d, $d, 180, 90)
    $path.AddArc($rect.Right - $d, $rect.Y, $d, $d, 270, 90)
    $path.AddArc($rect.Right - $d, $rect.Bottom - $d, $d, $d, 0, 90)
    $path.AddArc($rect.X, $rect.Bottom - $d, $d, $d, 90, 90)
    $path.CloseFigure()
    $brush = New-Object System.Drawing.Drawing2D.LinearGradientBrush $rect, ([System.Drawing.Color]::FromArgb(255, 77, 141, 255)), ([System.Drawing.Color]::FromArgb(255, 99, 70, 230)), 55
    $g.FillPath($brush, $path)
    # database cylinder
    $white = [System.Drawing.Color]::FromArgb(255, 255, 255, 255)
    $pen = New-Object System.Drawing.Pen $white, ([Math]::Max(1.0, $size * 0.045))
    $cx = $size * 0.5; $w = $size * 0.46; $top = $size * 0.2; $h = $size * 0.6; $e = $size * 0.15
    $x = $cx - $w / 2
    $g.DrawEllipse($pen, [single]$x, [single]$top, [single]$w, [single]$e)
    foreach ($k in 1, 2) {
        $y = $top + ($h * $k / 3.0)
        $g.DrawArc($pen, [single]$x, [single]$y, [single]$w, [single]$e, 0, 180)
    }
    $g.DrawLine($pen, [single]$x, [single]($top + $e / 2), [single]$x, [single]($top + $h + $e / 2))
    $g.DrawLine($pen, [single]($x + $w), [single]($top + $e / 2), [single]($x + $w), [single]($top + $h + $e / 2))
    $g.DrawArc($pen, [single]$x, [single]($top + $h), [single]$w, [single]$e, 0, 180)
    $g.Dispose()
    return $bmp
}

$sizes = 16, 24, 32, 48, 64, 128, 256
$pngs = @()
foreach ($s in $sizes) {
    $bmp = New-IconBitmap $s
    $ms = New-Object System.IO.MemoryStream
    $bmp.Save($ms, [System.Drawing.Imaging.ImageFormat]::Png)
    $pngs += , @($s, $ms.ToArray())
    if ($s -eq 256) { $bmp.Save((Join-Path $assets "icon.png"), [System.Drawing.Imaging.ImageFormat]::Png) }
    $bmp.Dispose()
}

$out = New-Object System.IO.MemoryStream
$bw = New-Object System.IO.BinaryWriter $out
$bw.Write([UInt16]0); $bw.Write([UInt16]1); $bw.Write([UInt16]$pngs.Count)
$offset = 6 + 16 * $pngs.Count
foreach ($p in $pngs) {
    $s = $p[0]; $data = $p[1]
    $dim = if ($s -ge 256) { 0 } else { $s }
    $bw.Write([byte]$dim); $bw.Write([byte]$dim); $bw.Write([byte]0); $bw.Write([byte]0)
    $bw.Write([UInt16]1); $bw.Write([UInt16]32)
    $bw.Write([UInt32]$data.Length); $bw.Write([UInt32]$offset)
    $offset += $data.Length
}
foreach ($p in $pngs) { $bw.Write([byte[]]$p[1]) }
[System.IO.File]::WriteAllBytes((Join-Path $assets "icon.ico"), $out.ToArray())
Write-Host "icon written"
