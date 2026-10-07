param([string]$A, [string]$B, [int]$Tol = 6)
# ASCII-only framing checker: is $B the same camera pose as $A?
# Reports vertical/horizontal shift, correlation, and PASS/FAIL.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

function Get-RowProf([string]$path, [int]$y0, [int]$y1, [int]$step) {
    $bmp = New-Object System.Drawing.Bitmap($path)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $len = $bd.Stride * $bmp.Height
    $bytes = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bytes, 0, $len)
    $bmp.UnlockBits($bd)
    $w = $bmp.Width; $h = $bmp.Height; $st = $bd.Stride
    $bmp.Dispose()
    $ys = @(); $vs = @()
    for ($y = $y0; $y -le $y1 -and $y -lt $h; $y += $step) {
        $sm = 0.0
        for ($x = 0; $x -lt $w; $x++) { $j = $y * $st + $x * 3; $sm += 0.2126*$bytes[$j+2] + 0.7152*$bytes[$j+1] + 0.0722*$bytes[$j] }
        $ys += $y; $vs += ($sm / $w)
    }
    return ,@{ ys = $ys; vs = $vs }
}

function Get-ColProf([string]$path, [int]$y0, [int]$y1, [int]$step) {
    $bmp = New-Object System.Drawing.Bitmap($path)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $len = $bd.Stride * $bmp.Height
    $bytes = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bytes, 0, $len)
    $bmp.UnlockBits($bd)
    $w = $bmp.Width; $h = $bmp.Height; $st = $bd.Stride
    $bmp.Dispose()
    $xs = @(); $vs = @()
    for ($x = 0; $x -lt $w; $x += $step) {
        $sm = 0.0; $c = 0
        for ($y = $y0; $y -le $y1 -and $y -lt $h; $y++) { $j = $y * $st + $x * 3; $sm += 0.2126*$bytes[$j+2] + 0.7152*$bytes[$j+1] + 0.0722*$bytes[$j]; $c++ }
        $xs += $x; $vs += ($sm / [Math]::Max(1, $c))
    }
    return ,@{ xs = $xs; vs = $vs }
}

# $pA/@$pB are arrays of values; index arrays are the coordinate (row or col).
function Score($va, $ca, $vb, $cb, [int]$d) {
    $n = $va.Length; $s = 0.0; $c = 0
    $sa = 0.0; $sb = 0.0; $n2 = 0.0; $n3 = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $t = $ca[$i] + $d
        $j = [Array]::IndexOf($cb, $t)
        if ($j -lt 0) { continue }
        $dx = $va[$i]; $dy = $vb[$j]
        $s += [Math]::Abs($dx - $dy); $c++
        $sa += $dx; $sb += $dy
    }
    if ($c -lt 5) { return @{ mad = 1e9; r = 0.0; c = 0 } }
    $ma = $sa / $c; $mb = $sb / $c
    $va2 = 0.0; $vb2 = 0.0; $cov = 0.0
    for ($i = 0; $i -lt $n; $i++) {
        $t = $ca[$i] + $d
        $j = [Array]::IndexOf($cb, $t)
        if ($j -lt 0) { continue }
        $x = $va[$i] - $ma; $y = $vb[$j] - $mb
        $cov += $x * $y; $va2 += $x * $x; $vb2 += $y * $y
    }
    $r = 0.0
    if ($va2 -gt 0 -and $vb2 -gt 0) { $r = $cov / [Math]::Sqrt($va2 * $vb2) }
    return @{ mad = ($s / $c); r = $r; c = $c }
}

$ra = Get-RowProf $A 40 340 2
$rb = Get-RowProf $B 40 340 2
$best = @{ mad = 1e9; r = 0.0 }; $bd = 0
$zero = $null
for ($d = -300; $d -le 300; $d++) {
    $sc = Score $ra.vs $ra.ys $rb.vs $rb.ys $d
    if ($d -eq 0) { $zero = $sc }
    if ($sc.c -ge 5 -and $sc.mad -lt $best.mad) { $best = $sc; $bd = $d }
}
Write-Output ("VERT  best dy = {0:+0;-0} px   MAD={1:N2} r={2:N3}   | dy=0: MAD={3:N2} r={4:N3}" -f $bd, $best.mad, $best.r, $zero.mad, $zero.r)

# compensate the vertical shift first, else the column band samples different content
$cb = Get-ColProf $B (40 + $bd) (500 + $bd) 4
$ca = Get-ColProf $A 40 500 4
$bestx = @{ mad = 1e9; r = 0.0 }; $bx = 0
$zerox = $null
for ($d = -240; $d -le 240; $d += 4) {
    $sc = Score $ca.vs $ca.xs $cb.vs $cb.xs $d
    if ($d -eq 0) { $zerox = $sc }
    if ($sc.c -ge 5 -and $sc.mad -lt $bestx.mad) { $bestx = $sc; $bx = $d }
}
Write-Output ("HORIZ best dx = {0:+0;-0} px   MAD={1:N2} r={2:N3}   | dx=0: MAD={3:N2} r={4:N3}" -f $bx, $bestx.mad, $bestx.r, $zerox.mad, $zerox.r)

# dy is measured from full-width row means (reliable: r=0.979 vs 0.772 above);
# dx comes from column means that still contain the debug field, so it is noisier -> wider tol.
$TolX = 30
$ok = ([Math]::Abs($bd) -le $Tol) -and ([Math]::Abs($bx) -le $TolX)
if ($ok) { Write-Output ("VERDICT: FRAMING OK - safe to compare across the two shots (|dy|<={0}px, |dx|<={1}px)" -f $Tol, $TolX) }
else {
    Write-Output ("VERDICT: FRAMING MISMATCH - do NOT compare across shots (|dy|<={0}px, |dx|<={1}px)" -f $Tol, $TolX)
    if ([Math]::Abs($bd) -gt $Tol) {
        $deg = [Math]::Abs($bd) / 1080.0 * 58.7155
        Write-Output ("  hint: content sits {0:N0} px lower in B than in A -> pitch camera {1} ~{2:N1} deg" -f $bd, $(if ($bd -gt 0) { 'DOWN' } else { 'UP' }), $deg)
    }
    if ([Math]::Abs($bx) -gt $TolX) { Write-Output ("  hint: horizontal offset {0} px" -f $bx) }
}
if ($ok) { exit 0 } else { exit 2 }
