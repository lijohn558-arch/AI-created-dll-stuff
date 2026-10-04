# quadcheck.ps1 -- quadrant content sampler for exported cube-face PNGs (ASCII-only output)
# Purpose: criterion-3 (face content) objective check -- samples each quadrant of every
#          face PNG and reports distinct-color counts plus the x511/x512 boundary pair
#          and the BR corner pixel.
# Usage  : powershell -NoProfile -ExecutionPolicy Bypass -File tools\quadcheck.ps1 -Scene S4f
# Output : %LOCALAPPDATA%\Temp\opencode\<scene>-quads.txt (ASCII, one line per face)
# Reads  : docs\analysis\<scene>-cube-ResourceId544-face*.png (exported by rdc_dump_cubefaces)
param(
  [string]$Scene = 'S4f',
  [string]$Repo = 'C:\Users\joker\skyrim-vulkan'
)
Add-Type -AssemblyName System.Drawing
$dir = Join-Path $Repo ('docs\analysis\' + $Scene + '-cube-ResourceId544-face*.png')
$files = Get-ChildItem $dir | Sort-Object Name
if (-not $files) { Write-Output ('no png found: ' + $dir); exit 1 }
$lines = @()
foreach ($f in $files) {
  $bmp = [System.Drawing.Bitmap]::FromFile($f.FullName)
  $w = $bmp.Width; $h = $bmp.Height; $hw = [int]($w / 2); $hh = [int]($h / 2)
  $q = @(@{}, @{}, @{}, @{})  # TL TR BL BR -> hashtable of color->count
  $step = 4
  $y = 0
  while ($y -lt $h) {
    $rowTop = ($y -lt $hh)
    $x = 0
    while ($x -lt $w) {
      $qi = 0
      if ($rowTop) { if ($x -ge $hw) { $qi = 1 } } else { if ($x -ge $hw) { $qi = 3 } else { $qi = 2 } }
      $c = $bmp.GetPixel($x, $y)
      $key = "$($c.R),$($c.G),$($c.B)"
      $q[$qi][$key] = 1
      $x += $step
    }
    $y += $step
  }
  # boundary probe on a content row (y=256): colors at x=511 and x=512
  $p511 = $bmp.GetPixel(511, 256); $p512 = $bmp.GetPixel(512, 256)
  # BR quadrant corner color (single color == clear color == unrendered quadrant)
  $cbr = $bmp.GetPixel($w - 1, $h - 1)
  $lines += ("{0} TL={1} TR={2} BL={3} BR={4} | x511=({5},{6},{7}) x512=({8},{9},{10}) | cornerBR=({11},{12},{13})" -f `
    $f.Name, $q[0].Count, $q[1].Count, $q[2].Count, $q[3].Count, `
    $p511.R, $p511.G, $p511.B, $p512.R, $p512.G, $p512.B, $cbr.R, $cbr.G, $cbr.B)
  $bmp.Dispose()
}
$out = Join-Path $env:LOCALAPPDATA ('Temp\opencode\' + $Scene + '-quads.txt')
[System.IO.File]::WriteAllLines($out, $lines, [System.Text.Encoding]::ASCII)
Write-Output ('written: ' + $out)
