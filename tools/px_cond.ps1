param([string]$Mask, [string]$Add, [switch]$SceneChecked, [switch]$Normal)
# px_cond.ps1 -- controlled comparison: read a probe shot CONDITIONED on the
#                hit/miss mask taken from a debug=2 shot of the SAME POSE.
#
# TWO MODES (same table, different question):
#   default        -Add = ssr.debug=8  -> measures the ssr.v1det ADDEND  (sec14.31 sec9)
#   -Normal        -Add = ssr.debug=0  -> measures the ORDINARY frame    (sec14.31 sec10 = P0)
#                    question: how far is the MISS region behind the HIT region in the
#                    NORMAL picture?  That gap IS issue B ("the fan area has no reflection"),
#                    and it is the baseline that ssr.edge=2 must close.
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
# RIGOROUS BOUND (default mode only): bleed <= 0.1 (588 <= 1.0, 585 share 0.9)
#   => y >= 0.10 can NEVER come from bleed alone, whatever the scene does.
#   y>=0.06 can (needs 588>=0.6).  Trust the y>=0.10 column first.
#   -Normal has NO such bound: there the miss group is a real picture, not a
#   control, so the footer reports a HIT/MISS ratio instead of a verdict about
#   addend.
#
# PIXEL VALUE = Rec.709 luma (0.2126 R + 0.7152 G + 0.0722 B), rounded to a byte
#   first, then /255.  This is NOT the same as reading G, and the difference is
#   small but real: a debug=8 pixel on screen is
#       screen = mix(grey_readout, 588_scene, 0.1) = 0.9*grey + 0.1*scene
#   and 588 IS COLOURED, so G carries a green-weighted scene bleed while luma
#   carries the true scene brightness (the same quantity the shader itself uses:
#   l8 = dot(add8, luma)).  Measured impact on an existing sec14.31 sec9 pair,
#   G -> luma:  HIT y>=.10 19.82% -> 20.39%, ratio 4.94 -> 5.11 (pose B);
#   14.24% -> 14.35%, ratio 4.36 -> 4.39 (pose A).  Verdicts unchanged.
#   G alone would also mis-rank a full-colour debug=0 frame (blue sky vs green
#   foliage), and -Normal reads exactly that, so one metric is used for both.
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
        # Rec.709 luma, rounded to a byte BEFORE dividing: on a grey debug=8 readout
        # 0.2126k+0.7152k+0.0722k rounds back to exactly k, so the bin index and every
        # threshold reproduce the old G-only numbers bit-for-bit.  On a full-colour
        # debug=0 frame G alone would mis-rank the pixels, which is why luma is used.
        $yl = [int][Math]::Round(0.2126 * [int]$pa[$i+2] + 0.7152 * [int]$pa[$i+1] + 0.0722 * [int]$pa[$i])
        if ($yl -lt 0) { $yl = 0 } elseif ($yl -gt 255) { $yl = 255 }
        $yy = $yl / 255.0
        if ($cls -eq 1) {
            $nHit++; $sumH += $yy
            if ($yy -ge 0.10) { $hiH++ }
            if ($yy -ge 0.06) { $loH++ }
            if ($yy -gt $maxH) { $maxH = $yy }
            $hh[$yl]++
        } else {
            $nMiss++; $sumM += $yy
            if ($yy -ge 0.10) { $hiM++ }
            if ($yy -ge 0.06) { $loM++ }
            if ($yy -gt $maxM) { $maxM = $yy }
            $hm[$yl]++
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
if ($Normal) {
    $hMean = $sumH / $nHit;            $mMean = $sumM / $nMiss
    $hP50  = Pctl $hh $nHit 0.50;       $mP50  = Pctl $hm $nMiss 0.50
    $hP90  = Pctl $hh $nHit 0.90;       $mP90  = Pctl $hm $nMiss 0.90
    $hP99  = Pctl $hh $nHit 0.99;       $mP99  = Pctl $hm $nMiss 0.99
    $rMean = if ($hMean -gt 0.0001) { $mMean / $hMean } else { 0.0 }
    $rP50  = if ($hP50  -gt 0.0001) { $mP50  / $hP50  } else { 0.0 }
    $hStr  = $hP90 - $hP50
    $mStr  = $mP90 - $mP50
    $rStr  = if ($hStr -gt 0.0001) { $mStr / $hStr } else { 0.0 }
    Write-Output "NORMAL-FRAME baseline for ISSUE B (queue #2, ssr.edge=2 must close this gap):"
    Write-Output ("  mean   hit " + ("{0,7:N4}" -f $hMean) + "   miss " + ("{0,7:N4}" -f $mMean) + "   miss/hit = " + ("{0,6:N3}" -f $rMean))
    Write-Output ("  p50    hit " + ("{0,7:N4}" -f $hP50)  + "   miss " + ("{0,7:N4}" -f $mP50)  + "   miss/hit = " + ("{0,6:N3}" -f $rP50))
    Write-Output ("  p90    hit " + ("{0,7:N4}" -f $hP90)  + "   miss " + ("{0,7:N4}" -f $mP90)  + "   p99 hit " + ("{0,6:N4}" -f $hP99) + "  miss " + ("{0,6:N4}" -f $mP99))
    Write-Output ("  structure (p90-p50)  hit " + ("{0,7:N4}" -f $hStr) + "   miss " + ("{0,7:N4}" -f $mStr) + "   miss/hit = " + ("{0,6:N3}" -f $rStr))
    Write-Output ""
    Write-Output "  p50 = brightness of the water (how much reflection is there at all)."
    Write-Output "  structure = how much DETAIL/contrast (a real cubemap is structured, an"
    Write-Output "  empty 585 fallback is flat).  HIT group = the SSR-reflecting band, MISS"
    Write-Output "  group = the near field + screen sides that have no reflection today."
    Write-Output ""
    Write-Output "  (The y>=.06 / y>=.10 columns are the BLEED bound of the debug=8 addend"
    Write-Output "   readout - on a normal debug=0 frame both groups are near 100% and the"
    Write-Output "   columns carry no information.  Judge -Normal on p50 / structure only.)"
    Write-Output ""
    Write-Output "CAVEAT: the two groups are DIFFERENT WATER reflecting DIFFERENT but"
    Write-Output "  LEGITIMATE surroundings - near water is supposed to reflect the dark near"
    Write-Output "  bank.  So miss/hit OVERSTATES how much of the gap is missing reflection:"
    Write-Output "  it is an UPPER BOUND and a BASELINE, never a target."
    Write-Output ""
    if ($rP50 -ge 0.90 -and $rStr -ge 0.60) {
        Write-Output ("VERDICT: B is SMALL (p50 ratio " + ("{0:N3}" -f $rP50) + ", structure ratio " + ("{0:N3}" -f $rStr) + ").")
        Write-Output "         The miss region already looks close to the hit region in the NORMAL"
        Write-Output "         frame, i.e. v0.18.15 (A) has already flattened most of the fan edge."
        Write-Output "         QUEUE #2 PRIORITY: LOW - do not spend the cross-API probe work on it"
        Write-Output "         until a user-visible complaint survives this number."
    } else {
        Write-Output ("VERDICT: B CONFIRMED (p50 ratio " + ("{0:N3}" -f $rP50) + ", structure ratio " + ("{0:N3}" -f $rStr) + ").")
        Write-Output "         The miss region is darker AND flatter than the hit region in the"
        Write-Output "         NORMAL frame = issue B is real and still visible after A."
        Write-Output "         ACCEPTANCE for ssr.edge=2 is PAIRED, same pose, edge=0 vs edge=2:"
        Write-Output "           1. HIT row must stay PUT - regression guard for issue A."
        Write-Output "           2. MISS row must MOVE - run this command once per shot against the"
        Write-Output "              SAME debug=2 mask and diff the two MISS rows (p50, structure)."
        Write-Output "           3. Do NOT require miss/hit to reach 1.0: a correct cubemap may"
        Write-Output "              legitimately stay dark where the scene is dark.  The complaint is"
        Write-Output "              an artificial EDGE, so judge the fan boundary visually too."
    }
    Write-Output ""
    Write-Output "NOTE: the hit/miss SPLIT itself is pure geometry (ssr.edge/mode/camera) and"
    Write-Output "      does NOT change with edge=2 - do not use px_hitmiss numbers as acceptance."
    exit 0
}
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
