# Store-sized copies of the player's screenshots
# (packaging/windows/store-listing.md, "Screenshots").
#
#   powershell -ExecutionPolicy Bypass -File scripts/store-screenshots.ps1 <png>...
#   ... -Out <dir>       default build\win\package\store
#
# The Microsoft Store takes desktop screenshots of 1366 x 768 or more,
# and the player's Ctrl+Alt+S shot is the guest's frame at the guest's
# own resolution (640 x 480, 800 x 600 ...). Each input is scaled by the
# smallest whole factor that reaches both minimums, with no smoothing
# (nearest neighbour), so every guest pixel becomes an n x n block and
# the picture is still the machine's frame, not a blurred one. An input
# already large enough is copied as it is. Output: <name>-store.png.
param(
  [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Png,
  [string]$Out
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$root = Split-Path -Parent $PSScriptRoot
if (-not $Out) { $Out = Join-Path $root 'build\win\package\store' }
New-Item -ItemType Directory -Force $Out | Out-Null
$minW = 1366; $minH = 768; $maxW = 3840; $maxH = 2160

foreach ($p in $Png) {
  $path = (Resolve-Path $p).Path
  $src = [System.Drawing.Image]::FromFile($path)
  try {
    $fx = [math]::Ceiling($minW / $src.Width); $fy = [math]::Ceiling($minH / $src.Height)
    $f = [int][math]::Max(1, [math]::Max($fx, $fy))
    [int]$w = $src.Width * $f; [int]$h = $src.Height * $f
    if ($w -gt $maxW -or $h -gt $maxH) {
      throw "store-screenshots: $p at x$f is $w x $h, over the Store's $maxW x $maxH"
    }
    $dest = Join-Path $Out ([IO.Path]::GetFileNameWithoutExtension($path) + '-store.png')
    if ($f -eq 1) {
      Copy-Item $path $dest -Force
    } else {
      $bmp = New-Object System.Drawing.Bitmap $w, $h
      $g = [System.Drawing.Graphics]::FromImage($bmp)
      $g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::NearestNeighbor
      $g.PixelOffsetMode = [System.Drawing.Drawing2D.PixelOffsetMode]::Half
      $g.DrawImage($src, 0, 0, $w, $h)
      $g.Dispose()
      $bmp.Save($dest, [System.Drawing.Imaging.ImageFormat]::Png)
      $bmp.Dispose()
    }
    "$($src.Width)x$($src.Height) x$f -> ${w}x${h}  $dest"
  } finally { $src.Dispose() }
}
