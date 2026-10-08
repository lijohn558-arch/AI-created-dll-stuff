param([string]$Mask, [string]$Add, [switch]$SceneChecked)
# px_cond.ps1 -- controlled comparison: read the debug=8 addend CONDITIONED on the
#                hit/miss mask taken from a debug=2 shot of the SAME POSE.
#
# WHY THIS IS THE RIGHT EXPERIMENT (docs/02 sec14.31 sec8/9):
#   debug=2 gives a perfect mask:   miss  =>  add == 0   BY CONSTRUCTION
#                                   hit   =>  add may be nonzero
#   debug=8 shows, on screen:       y = 0.9*add + 0.1*588        (585 share ~90%)
#   So the MISS group is a built-in CONTROL: its y distribution IS the bleed
#   (0.1*588, scene-dependent, NOT a constant - earlier shots made it look like a
#   fixed floor of l=0.0324 only because that region's 588 was uniformly dark).
#   The HIT group is the treatment.  addend exists only if the hit distribution
#   sits above the miss distribution.  Comparing hit against a hard-coded
#   threshold would silently credit the bleed as addend.
#
# RIGOROUS BOUND: bleed <= 0.1 (588 <= 1.0, 585 share 0.9) => y >= 0.10 can NEVER
#   come from bleed alone, whatever the scene does.  y>=0.06 can (needs 588>=0.6).
#   Both are reported; trust the y>=0.10 column first.
#
# REQUIRES THE TWO SHOTS TO SHARE A POSE.  check_frame is USELESS across debug
# modes (it correlates the whole frame, and the water inverts black<->red/green,
# which produces a fake dy=+256 r=0.87 alignment).  This script therefore refuses
# to run unless you pass -SceneChecked, i.e. after you aligned the two shots on
# the SCENE band (y 40..280, above the waterline) yourself.
#
# ASCII-only; CRLF no BOM (tools convention).  exit 0 = ok, 2 = bad input/refused.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if (-not $SceneChecked) {
    Write-Output "REFUSED: pass -SceneChecked after verifying both shots share a pose."
    Write-Output "         check_frame CANNOT do this across debug modes (water inverts),"
    Write-Output "         align the scene band (y40..280) instead."
    exit 2
}
if (-not (Test-Path -LiteralPath $Mask)) { Write-Output ("MISSING mask " + $Mask); exit 2 }
if (-not (Test-Path -LiteralPath $Add))  { Write-Output ("MISSING addend " + $Add); exit 2 }

function Load32([string]$p) {
    $src = [System.Drawing.Bitmap]::FromFile($p)
    $bm = New-Object System.Drawing.Bitmap($src.Width, $src.Height, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bm)
    $g.DrawImage($src, 0, 0, $src.Width, $src.Height)
    $g.Dispose(); $src.Dispose()
    $rect = New-Object System.Drawing.Rectangle 0, 0, $bm.Width, $bm.Height
    $bd = $bm.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $len = $bd.Stride * $bm.Height
    $buf = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $buf, 0, $len)
    $bm.UnlockBits($bd); $bm.Dispose()
    return ,$buf
}

$pm = Load32 $Mask
$pa = Load32 $Add
$W = 1920; $H = 1080
if ($pm.Length -ne $W * $H * 4 -or $pa.Length -ne $W * $H * 4) { Write-Output "UNEXPECTED image size"; exit 2 }

Write-Output ("mask  " + (Get-Item -LiteralPath $Mask).Name)
Write-Output ("add   " + (Get-Item -LiteralPath $Add).Name)
Write-Output ""
Write-Output "classification: G-R >= +0.15 => HIT,  G-R <= -0.15 => MISS,  else scene (dropped)"

$Tol = 0.15
$nHit = 0L; $nMiss = 0L
$sumH = 0.0; $sumM = 0.0
$hiH = 0L; $hiM = 0L          # y >= 0.10  (bleed-proof)
$loH = 0L; $loM = 0L          # y >= 0.06  (may be bleed)
$maxH = 0.0; $maxM = 0.0
# coarse histograms (256 bins of y) for percentiles
$hh = New-Object 'long[]' 256
$hm = New-Object 'long[]' 256

for ($y = 0; $y -lt $H; $y++) {
    $row = $y * $W * 4
    for ($x = 0; $x -lt $W; $x++) {
        $i = $row + $x * 4
        $d = ([int]$pm[$i+1] - [int]$pm[$i+2]) / 255.0     # G-R of the MASK shot
        $cls = 0
        if ($d -ge $Tol) { $cls = 1 } elseif ($d -le -$Tol) { $cls = 2 }
        if ($cls -eq 0) { continue }
        $yy = [int]$pa[$i+1] / 255.0                        # G of the ADDEND shot (grey anyway)
        if ($cls -eq 1) {
            $nHit++; $sumH += $yy
            if ($yy -ge 0.10) { $hiH++ }
            if ($yy -ge 0.06) { $loH++ }
            if ($yy -gt $maxH) { $maxH = $yy }
            $hh[[Math]::Min(255, [int]($yy * 255))]++
        } else {
            $nMiss++; $sumM += $yy
            if ($yy -ge 0.10) { $hiM++ }
            if ($yy -ge 0.06) { $loM++ }
            if ($yy -gt $maxM) { $maxM = $yy }
            $hm[[Math]::Min(255, [int]($yy * 255))]++
        }
    }
}

if ($nHit -lt 1000 -or $nMiss -lt 1000) {
    Write-Output ("NOT ENOUGH PIXELS (hit=" + $nHit + " miss=" + $nMiss + ") - wrong pair of shots?")
    exit 2
}

function Pctl([long[]]$h, [long]$n, [double]$q) {
    $t = $q * $n; $acc = 0L
    for ($v = 0; $v -lt 256; $v++) { $acc += $h[$v]; if ($acc -ge $t) { return ($v / 255.0) } }
    return 1.0
}

Write-Output ""
Write-Output ("group      n        mean y      p50 y      p90 y      p99 y     max y    y>=.06     y>=.10")
foreach ($g in @(@(1,'HIT '), @(2,'MISS'))) {
    $c = $g[0]; $nm = $g[1]
    if ($c -eq 1) {
        $n=$nHit; $s=$sumH; $h=$hh; $mx=$maxH; $l=$loH; $hi=$hiH
    } else {
        $n=$nMiss; $s=$sumM; $h=$hm; $mx=$maxM; $l=$loM; $hi=$hiM
    }
    Write-Output ($nm.PadRight(10) + $n.ToString().PadLeft(10) +
        ("{0,11:N4}" -f ($s/$n)).PadLeft(11) +
        ("{0,10:N4}" -f (Pctl $h $n 0.50)).PadLeft(10) +
        ("{0,10:N4}" -f (Pctl $h $n 0.90)).PadLeft(10) +
        ("{0,10:N4}" -f (Pctl $h $n 0.99)).PadLeft(10) +
        ("{0,9:N4}" -f $mx).PadLeft(9) +
        ("{0,9:N2}%" -f (100.0*$l/$n)).PadLeft(9) +
        ("{0,9:N2}%" -f (100.0*$hi/$n)).PadLeft(9))
}
Write-Output ""
Write-Output "MISS = control: its screen value is 0.9*0 + 0.1*588, i.e. PURE BLEED."
Write-Output "HIT  = treatment.  Only a HIT figure above the MISS figure is addend."

Write-Output ""
$dMean = ($sumH/$nHit) - ($sumM/$nMiss)
$r10 = if ($hiM -gt 0) { (100.0*$hiH/$nHit) / (100.0*$hiM/$nMiss) } else { [double]::PositiveInfinity }
if ($dMean -le 0.002 -and $r10 -lt 2.0) {
    Write-Output ("VERDICT: NO addend on HIT pixels either (dMean=" + ("{0:N4}" -f $dMean) +
                  ", y>=.10 ratio=" + ("{0:N2}" -f $r10) + ").")
    Write-Output "         hit is innocent too -> killer is bf^2 or max(baseRGB-refl_ssr,0)."
    Write-Output "         Next: debug=9 three-factor readout (R=gb / G=wSsr / B=max())."
} else {
    Write-Output ("VERDICT: addend IS present on HIT pixels (dMean=" + ("{0:N4}" -f $dMean) +
                  ", y>=.10 ratio=" + ("{0:N2}" -f $r10) + ").")
    Write-Output "         ssr.v1det works wherever it can; the remaining gap is the MISS region,"
    Write-Output "         which is queue #2's B problem (miss -> real cubemap), v0.18.16."
}
exit 0
