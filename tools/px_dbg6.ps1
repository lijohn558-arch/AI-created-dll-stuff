# px_dbg6.ps1 -- quantify the ssr.debug=6 displacement-field shot
# (docs/02 sec14.30.5). ASCII-only, no BOM needed.
#
# what it answers:
#   1. is the field BIMODAL (all clipped to 0/1) or smoothly graded?
#      -> histogram: %zero / %full / %mid
#   2. does the field carry the lambda~30 wave, or fine speckle?
#      -> band-limited averaged ACF on BAND(41,9)  and BAND(21,3)
#   3. how big are the white cells? -> mean row run-length of >=250
param(
    [string]$Path = "C:\Users\joker\skyrim-vulkan\docs\analysis\Screenshot_ssr.debug=6.png"
)

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @"
using System;
public class Df {
    public static double[] LumaXY(byte[] b, int stride, int x0, int x1, int y0, int y1, bool byRow) {
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
            for (int k = -h; k <= h; k++) { int j = i + k; if (j < 0) j = 0; if (j >= n) j = n - 1; s += a[j]; c++; }
            o[i] = s / c;
        }
        return o;
    }
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
        double ms = v / n;
        for (int L = 0; L <= lmax; L++) {
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

function Avg-AcfRaw($p, $box, [bool]$byRow, [int]$step, [int]$hpw, [int]$lpw, [int]$lmax, [bool]$raw) {
    $sum = New-Object 'double[]' ($lmax + 1)
    $cnt = 0
    if ($byRow) {
        for ($y = $box[2]; $y -le $box[3]; $y += $step) {
            $a = [Df]::LumaXY($p.b, $p.stride, $box[0], $box[1], $y, $y, $true)
            if (-not $raw) { $a = [Df]::BAND($a, $hpw, $lpw) }
            $r = [Df]::ACF($a, $lmax)
            for ($L = 0; $L -le $lmax; $L++) { $sum[$L] += $r[$L] }
            $cnt++
        }
    } else {
        for ($x = $box[0]; $x -le $box[1]; $x += $step) {
            $a = [Df]::LumaXY($p.b, $p.stride, $x, $x, $box[2], $box[3], $false)
            if (-not $raw) { $a = [Df]::BAND($a, $hpw, $lpw) }
            $r = [Df]::ACF($a, $lmax)
            for ($L = 0; $L -le $lmax; $L++) { $sum[$L] += $r[$L] }
            $cnt++
        }
    }
    $avg = New-Object 'double[]' ($lmax + 1)
    if ($cnt -gt 0) { for ($L = 0; $L -le $lmax; $L++) { $avg[$L] = $sum[$L] / $cnt } }
    return ,@{ c = $avg; n = $cnt }
}

# first local min = ACF trough (lambda/2 for a sine)
function Trough-Lag($r, $lmax) {
    for ($L = 2; $L -lt $lmax; $L++) {
        if ($r[$L] -lt $r[$L - 1] -and $r[$L] -le $r[$L + 1]) { return $L }
    }
    return 0
}
function Peak-Lag($r, $lmax) {
    $sawMin = $false
    for ($L = 2; $L -lt $lmax; $L++) {
        if (-not $sawMin) { if ($r[$L] -lt $r[$L - 1]) { $sawMin = $true }; continue }
        if ($r[$L] -ge $r[$L - 1] -and $r[$L - 1] -le $r[$L - 2]) { return $L }
    }
    return 0
}

if (-not (Test-Path $Path)) { Write-Output ("MISSING " + $Path); exit 1 }
$p = Get-Bytes $Path
Write-Output ("==== px_dbg6: displacement field histogram + ACF ====  " + (Split-Path $Path -Leaf))
Write-Output ("image {0}x{1}" -f $p.w, $p.h)

$boxes = @(
    @{ tag = "far   x200-1700  y210-380"; b = @(200, 1700, 210, 380) },
    @{ tag = "midR  x1050-1850 y430-700"; b = @(1050, 1850, 430, 700) },
    @{ tag = "nearL x60-420    y760-1070"; b = @(60, 420, 760, 1070) },
    @{ tag = "nearR x1450-1900 y760-1070"; b = @(1450, 1900, 760, 1070) }
)

foreach ($bx in $boxes) {
    $b = $bx.b
    $zero = 0; $full = 0; $mid = 0; $n = 0; $sm = 0.0
    $runs = New-Object System.Collections.Generic.List[int]
    for ($y = $b[2]; $y -le $b[3]; $y += 2) {
        $a = [Df]::LumaXY($p.b, $p.stride, $b[0], $b[1], $y, $y, $true)
        $run = 0
        for ($i = 0; $i -lt $a.Length; $i++) {
            $v = $a[$i]; $n++; $sm += $v
            if ($v -le 3.0) { $zero++ } elseif ($v -ge 252.0) { $full++ } else { $mid++ }
            if ($v -ge 250.0) { $run++ } else { if ($run -gt 0) { $runs.Add($run); $run = 0 } }
        }
        if ($run -gt 0) { $runs.Add($run) }
    }
    $ml = 0; if ($runs.Count -gt 0) { $ml = $full / $runs.Count }
    Write-Output ("---- {0}" -f $bx.tag)
    Write-Output ("     mean={0:N1}  zero%={1:P1}  full%={2:P1}  mid%={3:P1}  whiteRuns={4} meanRunLen={5:N1}px" -f `
        ($sm / $n), ($zero / $n), ($full / $n), ($mid / $n), $runs.Count, $ml)

    # wave band (9..41) and fine band (3..21), vertical scans
    $vw = Avg-AcfRaw $p $b $false 4 41 9 48 $false
    $tw = Trough-Lag $vw.c 48; $pw = Peak-Lag $vw.c 48
    $hf = Avg-AcfRaw $p $b $true 4 41 9 48 $false
    $tf = Trough-Lag $hf.c 48; $pf = Peak-Lag $hf.c 48
    Write-Output ("     ACF BAND(9-41): VERT trough={0} peak={1}  HORIZ trough={2} peak={3}" -f $tw, $pw, $tf, $pf)
    $vs = @(); for ($L = 1; $L -le 40; $L++) { $vs += ("{0}:{1:N2}" -f $L, $vw.c[$L]) }
    Write-Output ("     acfV " + ($vs -join " "))
    $hs = @(); for ($L = 1; $L -le 40; $L++) { $hs += ("{0}:{1:N2}" -f $L, $hf.c[$L]) }
    Write-Output ("     acfH " + ($hs -join " "))
    # fine band (3..21): is the field dominated by speckle?
    $fv = Avg-AcfRaw $p $b $false 4 21 3 24 $false
    $fs = @(); for ($L = 1; $L -le 20; $L++) { $fs += ("{0}:{1:N2}" -f $L, $fv.c[$L]) }
    Write-Output ("     acfF " + ($fs -join " "))
    # RAW (no band) -- what period is actually dominant in the field?
    $rv = Avg-AcfRaw $p $b $false 4 0 0 48 $true
    $tr = Trough-Lag $rv.c 48
    $rs = @(); for ($L = 1; $L -le 40; $L++) { $rs += ("{0}:{1:N2}" -f $L, $rv.c[$L]) }
    Write-Output ("     acfRAW V trough=" + $tr + "  " + ($rs -join " "))
    $rh = Avg-AcfRaw $p $b $true 4 0 0 48 $true
    $th = Trough-Lag $rh.c 48
    $rhs = @(); for ($L = 1; $L -le 40; $L++) { $rhs += ("{0}:{1:N2}" -f $L, $rh.c[$L]) }
    Write-Output ("     acfRAW H trough=" + $th + "  " + ($rhs -join " "))
    # WIDE band 9..121 -- catches scales the 41px window cut off
    $wv2 = Avg-AcfRaw $p $b $false 4 121 9 60 $false
    $wv2s = @(); for ($L = 1; $L -le 48; $L++) { $wv2s += ("{0}:{1:N2}" -f $L, $wv2.c[$L]) }
    Write-Output ("     acfW(9-121) " + ($wv2s -join " "))
}
Write-Output "done"
