param([string]$Path, [int]$TopN = 0, [switch]$Mark)
# px_v1det.ps1  -- reader for ssr.debug=8 screenshots (v0.18.15 diagnostic).
#
# debug=8 renders the UNIT addend:  add = max(baseRGB-refl,0) * (gb * wSsr)
#   (the same terms as the composite formula, but WITHOUT multiplying ssr.v1det)
# through a Reinhard tone map:   y = l/(1+l)   =>   l = y/(1-y)
# where l is the HDR luminance that det=1 would put back.
#
# So one screenshot answers "where can ssr.v1det add, and how much" with zero
# cross-image noise (no pose / lighting / water-animation dependence).
#
# Outputs: per-box histogram stats (already inverted to l), plus an ASCII heat
# map of the max-addend field so the bright-spot regions are visible at a glance.
#
# -Mark : also write "<name>.boxes.png" with the four measurement boxes drawn on
#         the shot.  The boxes are pose-FIXED (framed for the v0.18.14 pose), so
#         whenever the camera moves they silently start measuring land / HUD
#         instead of water.  One glance at the marked image settles it; never
#         trust the numbers without it (see docs/02 sec14.31 sec6).
#
# ASCII-only; CRLF no BOM (see px_ripple.ps1 convention).
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

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

if (-not (Test-Path -LiteralPath $Path)) { Write-Output ("MISSING " + $Path); exit 2 }
$fi = Get-Item -LiteralPath $Path
Write-Output ("file " + $fi.LastWriteTime.ToString('MM-dd HH:mm:ss') + "  " + $fi.Length + " B  " + $fi.Name)

$px = Load32 $Path
$W = 1920; $H = 1080
if ($px.Length -ne $W * $H * 4) { Write-Output ("UNEXPECTED bytes " + $px.Length); exit 2 }

# invert the Reinhard map: l = y/(1-y).  8bit max 255 saturates => cap it
function Inv([double]$y) {
    if ($y -le 0.0) { return 0.0 }
    if ($y -ge 0.998) { return 999.0 }
    return $y / (1.0 - $y)
}

# boxes: flat stride-5 (name,x1,y1,x2,y2).  PowerShell flattens nested @() literals.
$bx = @(
    'far',    200,  210, 1700,  380,
    'midR',  1050,  430, 1850,  700,
    'nearL',   60,  760,  420, 1070,
    'nearR', 1450,  760, 1900, 1070,
    'ALL',       0,    0, 1920, 1080
)
$nb = [int]($bx.Count / 5)

$cnt  = New-Object long[] $nb
$nz   = New-Object long[] $nb
$sumY = New-Object double[] $nb
$maxY = New-Object double[] $nb
$maxX = New-Object long[] $nb
$maxYy = New-Object long[] $nb
$hist = New-Object 'long[,]' $nb, 256

$GW = 64; $GH = 24
$grid = New-Object 'double[,]' $GH, $GW
for ($gy = 0; $gy -lt $GH; $gy++) { for ($gx = 0; $gx -lt $GW; $gx++) { $grid[$gy,$gx] = 0.0 } }

for ($y = 0; $y -lt $H; $y++) {
    $row = $y * $W * 4
    $gy = [int][Math]::Floor($y * $GH / $H)    # NOTE: [int]() would ROUND (63.97 -> 64) and blow the array
    for ($x = 0; $x -lt $W; $x++) {
        $i = $row + $x * 4
        $v = $px[$i]          # B channel (image is grey anyway)
        $yy = $v / 255.0
        $gxc = [int][Math]::Floor($x * $GW / $W)
        if ($yy -gt $grid[$gy, $gxc]) { $grid[$gy, $gxc] = $yy }
        for ($bi = 0; $bi -lt $nb; $bi++) {
            $o = $bi * 5
            if ($x -ge $bx[$o+1] -and $x -lt $bx[$o+3] -and $y -ge $bx[$o+2] -and $y -lt $bx[$o+4]) {
                $cnt[$bi]++
                $hist[$bi, $v]++
                $sumY[$bi] += $yy
                if ($yy -gt 0.0) { $nz[$bi]++ }
                if ($yy -gt $maxY[$bi]) { $maxY[$bi] = $yy; $maxX[$bi] = $x; $maxYy[$bi] = $y }
                break
            }
        }
    }
}

Write-Output ""
Write-Output "NOTE: values are the UNIT addend (det=1 full scale).  Actual added luma at det=D is l*D."
Write-Output ""
$hdr = 'box     ' + 'n'.PadLeft(9) + 'nonzero'.PadLeft(10) + 'nz%'.PadLeft(9) + 'meanL'.PadLeft(10) + 'p99L'.PadLeft(10) + 'p99.9L'.PadLeft(10) + 'maxL'.PadLeft(11)
Write-Output $hdr
for ($bi = 0; $bi -lt $nb; $bi++) {
    $n = $cnt[$bi]
    $meanY = $sumY[$bi] / $n
    $p99 = -1.0; $p999 = -1.0
    $t99 = 0.99 * $n; $t999 = 0.999 * $n; $acc = 0.0
    for ($v = 0; $v -lt 256; $v++) {
        $c = $hist[$bi, $v]
        if ($c -eq 0) { continue }
        if ($p99 -lt 0.0 -and ($acc + $c) -ge $t99) { $p99 = $v / 255.0 }
        if ($p999 -lt 0.0 -and ($acc + $c) -ge $t999) { $p999 = $v / 255.0; break }
        $acc += $c
    }
    if ($p99 -lt 0.0) { $p99 = 255.0 / 255.0 }
    if ($p999 -lt 0.0) { $p999 = 255.0 / 255.0 }
    $row = ([string]$bx[$bi*5]).PadRight(7) + $n.ToString().PadLeft(9)
    $row += $nz[$bi].ToString().PadLeft(10)
    $row += ("{0,8:N3}%" -f (100.0 * $nz[$bi] / $n)).PadLeft(9)
    $row += ("{0,10:N4}" -f (Inv $meanY)).PadLeft(10)
    $row += ("{0,10:N4}" -f (Inv $p99)).PadLeft(10)
    $row += ("{0,10:N4}" -f (Inv $p999)).PadLeft(10)
    $row += ("{0,11:N3}" -f (Inv $maxY[$bi])).PadLeft(11)
    Write-Output $row
}

# Floor test.  The 585/588 composite puts a floor at l=0.0324 => y=0.0314 (byte 8).
# The heat map bins on y, so that floor lands in the '-' bin (y<0.06): a SEA OF '-' IS
# BLEED, NOT ADDEND.  Reading the map by eye therefore reports "looks like something"
# for a shot that adds nothing.  Only y>=0.06 (byte>=102) is real addend.  Read this.
Write-Output ""
Write-Output "floor test (588 bleed l=0.0324 => y=0.0314 => byte 8;  real addend = y>=0.06 = byte>=102):"
$fh = 'box     ' + '<=.001'.PadLeft(9) + '<.02'.PadLeft(9) + '<.06'.PadLeft(9) + '<.15'.PadLeft(9) +
      '<.40'.PadLeft(9) + '>=.40'.PadLeft(9) + 'ABOVE-FLOOR'.PadLeft(14)
Write-Output $fh
for ($bi = 0; $bi -lt $nb; $bi++) {
    $n = $cnt[$bi]
    $b0 = 0L; $b1 = 0L; $b2 = 0L; $b3 = 0L; $b4 = 0L; $b5 = 0L
    for ($v = 0; $v -lt 256; $v++) {
        $c = $hist[$bi, $v]
        if ($c -eq 0) { continue }
        if ($v -eq 0) { $b0 += $c }
        elseif ($v -le 5)   { $b1 += $c }
        elseif ($v -le 15)  { $b2 += $c }
        elseif ($v -le 38)  { $b3 += $c }
        elseif ($v -le 101) { $b4 += $c }
        else                { $b5 += $c }
    }
    $row = ([string]$bx[$bi*5]).PadRight(7)
    $row += ("{0,8:N2}%" -f (100.0 * $b0 / $n)).PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $b1 / $n)).PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $b2 / $n)).PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $b3 / $n)).PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $b4 / $n)).PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $b5 / $n)).PadLeft(9)
    $row += ("{0,13:N2}%" -f (100.0 * ($b3 + $b4 + $b5) / $n)).PadLeft(14)
    Write-Output $row
}
Write-Output "CAVEAT: ABOVE-FLOOR is the only number here that means 'this shot actually added light'."
Write-Output "        nz% above counts every byte >0, i.e. the bleed itself, and must never be used as"
Write-Output "        'fraction with addend'.  A water box at ~0% is a NEGATIVE result, not a bad box."
Write-Output "CAVEAT: these buckets read the B channel, same as the heat map above (so map and table"
Write-Output "        always agree).  debug=8's own output IS grey, but the screen shows mix(585,588,w)"
Write-Output "        and 588 is COLOURED scene -> on bleed-only pixels B != luma.  Water boxes are"
Write-Output "        insensitive to that choice (verified: nearL 3.01% / nearR 0.00% under both B and"
Write-Output "        luma); divergences show up only in coloured scene pixels (far, the rock in midR)."

Write-Output ""
Write-Output "CAVEAT: the four boxes are pose-FIXED (framed for the v0.18.14 shot).  Once the camera"
Write-Output "        moves they measure land / HUD instead of water and the numbers above are garbage."
Write-Output "        Re-run with -Mark to draw them on the shot and confirm before believing anything."
Write-Output "CAVEAT: 585 is composited ONLY on water, so off-water pixels show the SCENE, not the addend."
Write-Output "        Those '#' in the upper heat-map rows are rock/sky brightness - ignore them entirely."
Write-Output "        And the readings are an upper bound: screen y = 0.9*add + 0.1*588 (585 share ~90%)."
Write-Output "        So nz% is inflated by the 588 bleed and must not be read as 'fraction with addend'."

Write-Output ""
Write-Output "det=0.3 would add meanL*0.3 to the whole box; maxL locates the hottest pixel:"
for ($bi = 0; $bi -lt $nb; $bi++) {
    $m = Inv ($sumY[$bi] / $cnt[$bi])
    $nm = [string]$bx[$bi*5]
    Write-Output ("  " + $nm.PadRight(7) + " meanL=" + ("{0,9:N5}" -f $m) +
        "   det=0.3 => " + ("{0,9:N5}" -f ($m * 0.3)) +
        "   max at x" + $maxX[$bi] + " y" + $maxYy[$bi])
}

if ($TopN -gt 0) {
    Write-Output ""
    Write-Output ("top " + $TopN + " cells (cell = " + [int](1920/$GW) + "x" + [int](1080/$GH) + " px):")
    $al = New-Object System.Collections.ArrayList
    for ($gy = 0; $gy -lt $GH; $gy++) {
        for ($gx = 0; $gx -lt $GW; $gx++) {
            [void]$al.Add(@($grid[$gy,$gx], $gx, $gy))
        }
    }
    $sorted = @($al | Sort-Object -Property @{Expression = { $_[0] }; Descending = $true} | Select-Object -First $TopN)
    foreach ($c in $sorted) {
        $yy = [double]$c[0]; $gx = [int]$c[1]; $gy = [int]$c[2]
        $x0 = [int](($gx * 1920) / $GW); $y0 = [int]($gy * 1080 / $GH)
        $cw = [int](1920 / $GW); $chh = [int](1080 / $GH)
        Write-Output ("  cell(" + $gx.ToString().PadLeft(2) + "," + $gy.ToString().PadLeft(2) + ")" +
            "  px x" + $x0.ToString().PadLeft(4) + "-" + ($x0 + $cw).ToString().PadLeft(4) +
            " y" + $y0.ToString().PadLeft(4) + "-" + ($y0 + $chh).ToString().PadLeft(4) +
            "   y=" + ("{0,7:N4}" -f $yy) + "   l=" + ("{0,9:N3}" -f (Inv $yy)))
    }
}

Write-Output ""
Write-Output "max-addend heat map (rows=top->bottom).  ' '=none  .=tiny  -=small  +=mid  *=high  #=sat"
for ($gy = 0; $gy -lt $GH; $gy++) {
    $line = ''
    for ($gx = 0; $gx -lt $GW; $gx++) {
        $yy = $grid[$gy, $gx]
        $ch = '#'
        if ($yy -le 0.001) { $ch = ' ' }
        elseif ($yy -lt 0.02) { $ch = '.' }
        elseif ($yy -lt 0.06) { $ch = '-' }
        elseif ($yy -lt 0.15) { $ch = '+' }
        elseif ($yy -lt 0.40) { $ch = '*' }
        $line += $ch
    }
    Write-Output $line
}
Write-Output ""
Write-Output "VERDICT: blank map => gb gate or baseRGB<=refl is killing the addend everywhere."
Write-Output ("         '*'/'#' clustered on the water => det restores highlights there; read p99L to size it.")

if ($Mark) {
    $src = [System.Drawing.Bitmap]::FromFile($Path)
    $g = [System.Drawing.Graphics]::FromImage($src)
    $g.SmoothingMode = [System.Drawing.Drawing2D.SmoothingMode]::AntiAlias
    $pen = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 40, 40, 255), 4)
    $penB = New-Object System.Drawing.Pen([System.Drawing.Color]::FromArgb(255, 40, 255, 40), 4)
    $font = New-Object System.Drawing.Font('Consolas', 18, [System.Drawing.FontStyle]::Bold)
    $brB = [System.Drawing.Brushes]::Blue
    $brG = [System.Drawing.Brushes]::Lime
    for ($bi = 0; $bi -lt $nb; $bi++) {
        $o = $bi * 5
        $nm = [string]$bx[$o]
        if ($nm -eq 'ALL') { continue }
        $rx = [int]$bx[$o+1]; $ry = [int]$bx[$o+2]
        $rw = [int]($bx[$o+3] - $bx[$o+1]); $rh = [int]($bx[$o+4] - $bx[$o+2])
        # alternate colours so adjacent boxes stay tellable apart
        $useB = (($bi % 2) -eq 0)
        $p = if ($useB) { $pen } else { $penB }
        $b = if ($useB) { $brB } else { $brG }
        $g.DrawRectangle($p, $rx, $ry, $rw, $rh)
        $g.DrawString($nm + ' ' + $rw + 'x' + $rh, $font, $b, ($rx + 6), ($ry + 6))
    }
    $g.Dispose()
    $out = [System.IO.Path]::ChangeExtension($Path, '.boxes.png')
    $src.Save($out, [System.Drawing.Imaging.ImageFormat]::Png)
    $src.Dispose()
    Write-Output ""
    Write-Output ("MARKED -> " + $out)
}
