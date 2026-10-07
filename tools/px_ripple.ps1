# px_ripple.ps1 -- measure the ORIGINAL water ripple from an `ssr.debug=7` shot
# (docs/02 sec14.30.3): dominant wavelength via AVERAGED, BAND-LIMITED ACF
# (row & column scans), plus sparkle density / band contrast on two COMMON boxes
# shared by both screenshots (original layer vs our SSR output).
#
# why band-limit first: the 2px white sparkle glints dominate the variance and
# hide the wave -- BAND = MA(41) - MA(9) keeps scales 9..41px only.
# why averaged: a single row/col ACF is noise; averaging 55..161 scans exposes
# the damped oscillation (zero at lag~7, trough at lag~15, positive lobe ~29).
#
# usage:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\px_ripple.ps1
#   powershell ... -File tools\px_ripple.ps1 -Dir "C:\...\docs\analysis"
# ASCII-only output (no BOM needed).
param(
    [string]$Dir = "C:\Users\joker\skyrim-vulkan\docs\analysis"
)

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
public class Rp {
    public static double[] LumaXY(byte[] b, int stride, int x0, int x1, int y0, int y1, bool byRow, int step) {
        // byRow  : one row   y=y0, x0..x1-1
        // !byRow : one col   x=x0, y0..y1-1
        int n = byRow ? (x1 - x0) : (y1 - y0);
        double[] o = new double[n];
        for (int i = 0; i < n; i++) {
            int x = byRow ? (x0 + i) : x0;
            int y = byRow ? y0 : (y0 + i);
            int j = y * stride + x * 3;
            o[i] = 0.2126 * b[j + 2] + 0.7152 * b[j + 1] + 0.0722 * b[j];
        }
        return o;
    }
    public static double[] MA(double[] a, int w) {
        int n = a.Length; double[] o = new double[n]; int h = w / 2;
        for (int i = 0; i < n; i++) {
            double s = 0; int c = 0;
            for (int k = -h; k <= h; k++) {
                int j = i + k; if (j < 0) j = 0; if (j >= n) j = n - 1;
                s += a[j]; c++;
            }
            o[i] = s / c;
        }
        return o;
    }
    public static double[] HP(double[] a, int w) {
        double[] m = MA(a, w); double[] o = new double[a.Length];
        for (int i = 0; i < a.Length; i++) o[i] = a[i] - m[i];
        return o;
    }
    // band-pass: keep structures with scale in [lpw, hpw] -- kills the 2px sparkle glints
    // so the ACF reflects the WAVE instead of the speckle noise.
    public static double[] BAND(double[] a, int hpw, int lpw) {
        double[] hi = MA(a, hpw); double[] lo = MA(a, lpw);
        double[] o = new double[a.Length];
        for (int i = 0; i < a.Length; i++) o[i] = hi[i] - lo[i];
        return o;
    }
    public static double[] ACF(double[] a, int lmax) {
        int n = a.Length; double[] r = new double[lmax + 1];
        double m = 0; for (int i = 0; i < n; i++) m += a[i]; m /= n;
        double v = 0; for (int i = 0; i < n; i++) { double d = a[i] - m; v += d * d; }
        if (v <= 1e-9) return r;
        double ms = v / n;                 // mean square = gamma(0); r[L] must be normalized by THIS,
        for (int L = 0; L <= lmax; L++) {  // not by v (v = n * ms) or the whole curve collapses to ~1/n
            double s = 0; int c = 0;
            for (int i = 0; i + L < n; i++) { s += (a[i] - m) * (a[i + L] - m); c++; }
            r[L] = (c > 0 ? s / c : 0) / ms;
        }
        r[0] = 1.0;
        return r;
    }
}
"@

function Get-Bytes([string]$path) {
    $bmp = New-Object System.Drawing.Bitmap($path)
    $rect = New-Object System.Drawing.Rectangle(0, 0, $bmp.Width, $bmp.Height)
    $bd = $bmp.LockBits($rect, [System.Drawing.Imaging.ImageLockMode]::ReadOnly,
                        [System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $len = $bd.Stride * $bmp.Height
    $bytes = New-Object byte[] $len
    [System.Runtime.InteropServices.Marshal]::Copy($bd.Scan0, $bytes, 0, $len)
    $bmp.UnlockBits($bd)
    $r = @{ w = $bmp.Width; h = $bmp.Height; stride = $bd.Stride; b = $bytes }
    $bmp.Dispose()
    return $r
}

# average ACF over many scans; return the averaged curve
function Avg-Acf($p, $box, [bool]$byRow, [int]$step, [int]$hpw, [int]$lmax) {
    $sum = New-Object 'double[]' ($lmax + 1)
    $cnt = 0
    if ($byRow) {
        for ($y = $box[2]; $y -le $box[3]; $y += $step) {
            $a = [Rp]::LumaXY($p.b, $p.stride, $box[0], $box[1], $y, $y, $true, 0)
            $r = [Rp]::ACF([Rp]::BAND($a, $hpw, 9), $lmax)
            for ($L = 0; $L -le $lmax; $L++) { $sum[$L] += $r[$L] }
            $cnt++
        }
    } else {
        for ($x = $box[0]; $x -le $box[1]; $x += $step) {
            $a = [Rp]::LumaXY($p.b, $p.stride, $x, $x, $box[2], $box[3], $false, 0)
            $r = [Rp]::ACF([Rp]::BAND($a, $hpw, 9), $lmax)
            for ($L = 0; $L -le $lmax; $L++) { $sum[$L] += $r[$L] }
            $cnt++
        }
    }
    $avg = New-Object 'double[]' ($lmax + 1)
    if ($cnt -gt 0) { for ($L = 0; $L -le $lmax; $L++) { $avg[$L] = $sum[$L] / $cnt } }
    return ,@{ c = $avg; n = $cnt }
}

# first local max after the first local min = dominant period
function Peak-Lag($r, $lmax) {
    $sawMin = $false
    for ($L = 2; $L -lt $lmax; $L++) {
        if (-not $sawMin) {
            if ($r[$L] -lt $r[$L - 1]) { $sawMin = $true }
            continue
        }
        if ($r[$L] -ge $r[$L - 1] -and $r[$L - 1] -le $r[$L - 2]) { return $L }
    }
    return 0
}

$def = Get-ChildItem $Dir -Filter "Screenshot_v0.18.12 *.png" | Where-Object { $_.Name -notmatch "ssr" } | Select-Object -First 1
$z8 = Get-ChildItem $Dir -Filter "Screenshot_ssr.ripplesz=8*.png" | Select-Object -First 1
$files = @(
    @{ tag = "ORIG debug=7"; path = (Join-Path $Dir "Screenshot_ssr.debug=7.png") },
    @{ tag = "SSR  default"; path = $def.FullName }
)
if ($z8) { $files += @{ tag = "SSR  ripplesz=8"; path = $z8.FullName } }
# two COMMON boxes (same screen rect in both shots): right water / left water
$boxes = @(
    @{ tag = "boxR x1050-1850 y430-700"; b = @(1050, 1850, 430, 700) },
    @{ tag = "boxL x60-420   y470-900"; b = @(60, 420, 470, 900) }
)

Write-Output "==== px_ripple v2: averaged ACF wavelength + sparkle ===="
foreach ($f in $files) {
    if (-not (Test-Path $f.path)) { Write-Output ("MISSING " + $f.path); continue }
    $p = Get-Bytes $f.path
    Write-Output ("---- {0}   {1}" -f $f.tag, (Split-Path $f.path -Leaf))
    foreach ($bx in $boxes) {
        # 1. vertical scan (across horizontal crests), hp window 41
        $v = Avg-Acf $p $bx.b $false 5 41 48
        $lv = Peak-Lag $v.c 48
        $vs = @(); for ($L = 1; $L -le 40; $L++) { $vs += ("{0}:{1:N3}" -f $L, $v.c[$L]) }
        Write-Output ("    VERT {0}  n={1} peakLag={2}px peakVal={3:N3}" -f $bx.tag, $v.n, $lv, $v.c[$lv])
        Write-Output ("         acf " + ($vs -join " "))

        # 2. horizontal scan
        $h = Avg-Acf $p $bx.b $true 5 41 48
        $lh = Peak-Lag $h.c 48
        $hs = @(); for ($L = 1; $L -le 40; $L++) { $hs += ("{0}:{1:N3}" -f $L, $h.c[$L]) }
        Write-Output ("    HORIZ {0} n={1} peakLag={2}px peakVal={3:N3}" -f $bx.tag, $h.n, $lh, $h.c[$lh])
        Write-Output ("         acf " + ($hs -join " "))

        # 3. sparkle + HF contrast (hp9)
        $px = 0; $tot = 0; $runs = New-Object System.Collections.Generic.List[int]
        $ssq = 0.0; $sm = 0.0; $n = 0; $bsq = 0.0; $fsq = 0.0; $wsq = 0.0
        for ($y = $bx.b[2]; $y -le $bx.b[3]; $y += 4) {
            $a = [Rp]::LumaXY($p.b, $p.stride, $bx.b[0], $bx.b[1], $y, $y, $true, 0)
            $hp = [Rp]::HP($a, 9)
            $bd = [Rp]::BAND($a, 41, 9)
            $fn = [Rp]::BAND($a, 21, 3)    # fine   3..21px  (glint scale)
            $wv = [Rp]::BAND($a, 61, 21)   # wave  21..61px  (lambda ~30)
            $run = 0
            for ($i = 0; $i -lt $hp.Length; $i++) {
                $tot++; $n++; $sm += $a[$i]; $ssq += $hp[$i] * $hp[$i]; $bsq += $bd[$i] * $bd[$i]
                $fsq += $fn[$i] * $fn[$i]; $wsq += $wv[$i] * $wv[$i]
                if ($hp[$i] -gt 45) { $px++; $run++ } else { if ($run -gt 0) { $runs.Add($run); $run = 0 } }
            }
            if ($run -gt 0) { $runs.Add($run) }
        }
        $mean = $sm / $n
        $hfr = [math]::Sqrt($ssq / $n)
        $brms = [math]::Sqrt($bsq / $n)
        $frms = [math]::Sqrt($fsq / $n)
        $wrms = [math]::Sqrt($wsq / $n)
        $runStr = "-"; $mlen = 0
        if ($runs.Count -gt 0) { $mlen = $px / $runs.Count; $runStr = ("{0:N1}" -f $mlen) }
        Write-Output ("    HF    mean={0:N1} rmsHP9={1:N2}  band(9-41)={2:N2}  FINE(3-21)={3:N2}  WAVE(21-61)={4:N2}  fine/wave={5:N2}" -f `
            $mean, $hfr, $brms, $frms, $wrms, $(if ($wrms -gt 0.001) { $frms / $wrms } else { 0 }))
        Write-Output ("          sparkle={0} ({1:P2}) runs={2} meanLen={3}px" -f $px, ($px / $tot), $runs.Count, $runStr)
    }
}
Write-Output "done"
