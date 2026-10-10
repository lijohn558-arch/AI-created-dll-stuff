$ErrorActionPreference = "Stop"

# =====================================================================
# chain_verify.ps1 -- validate the v0.18.24 direction-chain math
# BEFORE touching any C++ (CI is the only compile gate).
#
# Ground truth: RenderDoc capture, ev=24082, VS slot0, ResourceId 1506
#   floats[0..11]  = V (world -> game view, row-major, orthogonal)
#   floats[16..31] = P (game projection, row-major, row3=(0,0,1,0))
#
# Claim (derived):
#   our shader view space = (x, y, -z) of game view space   [viewZ = -z_g]
#   d_world = V^T . D . Rf_ours ,  D = diag(1,1,-1)
#   camBlockOk (colMode, det<0) turns raw V into C = V . D
#   => chain = D . C^T . D  (proper) -> camQuat(chain) -> publish directly
# Old pipeline published quat(C) (+conj)  => must FAIL this test.
# =====================================================================

$j = Get-Content "C:\Users\joker\skyrim-vulkan\docs\analysis\skyrimse_frame_frame1664-cbuf-scan.json" -Raw -Encoding UTF8 | ConvertFrom-Json
$b = @($j.blocks | Where-Object { $_.ev -eq 24082 -and $_.stage -eq "VS" -and $_.slot -eq 0 } | Select-Object -First 1)
if (-not $b) { throw "block ev=24082 VS slot0 not found" }
$f = @($b.floats | ForEach-Object { [double]$_ })
if ($f.Count -lt 48) { throw "block too small: $($f.Count)" }

$V1 = New-Object double[] 9
# V is stored as 4x4 (16 floats); take top-left 3x3 = same packing as main.cpp candidate:
#   p = {m[0],m[1],m[2], m[4],m[5],m[6], m[8],m[9],m[10]}
$src = @($f[0], $f[1], $f[2], $f[4], $f[5], $f[6], $f[8], $f[9], $f[10])
for ($i = 0; $i -lt 9; $i++) { $V1[$i] = $src[$i] }

# ---- P block (float offset 16) ----
$PA = $f[16]          # A = 1/(aspect*tanHalfY)
$PB = $f[21]          # B = 1/tanHalfY
$PC = $f[26]          # C = far/(far-near)
$PD = $f[27]          # D = -C*near

# ========================= helpers ===================================

function Det3([double[]]$m) {
    return $m[0]*($m[4]*$m[8]-$m[5]*$m[7]) - $m[1]*($m[3]*$m[8]-$m[5]*$m[6]) + $m[2]*($m[3]*$m[7]-$m[4]*$m[6])
}

function Cross3([double[]]$a, [double[]]$b) {
    $r = New-Object double[] 3
    $r[0] = $a[1] * $b[2] - $a[2] * $b[1]
    $r[1] = $a[2] * $b[0] - $a[0] * $b[2]
    $r[2] = $a[0] * $b[1] - $a[1] * $b[0]
    return ,$r
}

# exact replica of shader line 515: v + 2*cross(qv, cross(qv,v) + w*v)
function RotQ([double[]]$q, [double[]]$v) {
    $qv = @($q[0], $q[1], $q[2])
    $c1 = Cross3 $qv $v
    # NOTE: never put binary arithmetic inside @() with commas (PS mis-groups
    # the comma list into the multiply operand); assign element-wise instead.
    $inner = New-Object double[] 3
    $inner[0] = $c1[0] + $q[3] * $v[0]
    $inner[1] = $c1[1] + $q[3] * $v[1]
    $inner[2] = $c1[2] + $q[3] * $v[2]
    $o = Cross3 $qv $inner
    $out = New-Object double[] 3
    $out[0] = $v[0] + 2.0 * $o[0]
    $out[1] = $v[1] + 2.0 * $o[1]
    $out[2] = $v[2] + 2.0 * $o[2]
    return ,$out
}

# exact replica of main.cpp camQuat (lines 2987-3028)
function CamQuat([double[]]$r) {
    $q = New-Object double[] 4
    $tr = $r[0] + $r[4] + $r[8]
    if ($tr -gt 0.0) {
        $s = [Math]::Sqrt($tr + 1.0) * 2.0
        $q[3] = 0.25 * $s
        $q[0] = ($r[7] - $r[5]) / $s
        $q[1] = ($r[2] - $r[6]) / $s
        $q[2] = ($r[3] - $r[1]) / $s
    } elseif ($r[0] -gt $r[4] -and $r[0] -gt $r[8]) {
        $s = [Math]::Sqrt(1.0 + $r[0] - $r[4] - $r[8]) * 2.0
        $q[3] = ($r[7] - $r[5]) / $s
        $q[0] = 0.25 * $s
        $q[1] = ($r[1] + $r[3]) / $s
        $q[2] = ($r[2] + $r[6]) / $s
    } elseif ($r[4] -gt $r[8]) {
        $s = [Math]::Sqrt(1.0 + $r[4] - $r[0] - $r[8]) * 2.0
        $q[3] = ($r[2] - $r[6]) / $s
        $q[0] = ($r[1] + $r[3]) / $s
        $q[1] = 0.25 * $s
        $q[2] = ($r[5] + $r[7]) / $s
    } else {
        $s = [Math]::Sqrt(1.0 + $r[8] - $r[0] - $r[4]) * 2.0
        $q[3] = ($r[3] - $r[1]) / $s
        $q[0] = ($r[2] + $r[6]) / $s
        $q[1] = ($r[5] + $r[7]) / $s
        $q[2] = 0.25 * $s
    }
    $n = [Math]::Sqrt($q[0]*$q[0] + $q[1]*$q[1] + $q[2]*$q[2] + $q[3]*$q[3])
    if (-not ($n -gt 0.5)) { return $null }
    for ($i = 0; $i -lt 4; $i++) { $q[$i] /= $n }
    return ,$q
}

# matrix multiply: C = A . B (row-major 9)
function Mul3([double[]]$a, [double[]]$b) {
    $c = New-Object double[] 9
    for ($i = 0; $i -lt 3; $i++) {
        for ($k = 0; $k -lt 3; $k++) {
            $acc = 0.0
            for ($jj = 0; $jj -lt 3; $jj++) { $acc += $a[$i*3+$jj] * $b[$jj*3+$k] }
            $c[$i*3+$k] = $acc
        }
    }
    return ,$c
}

# ===================== pipeline replica ==============================

$vecs = @(
    [double[]]@(0.0, 0.0, -1.0),
    [double[]]@(1.0, 0.0, 0.0),
    [double[]]@(0.0, 1.0, 0.0),
    [double[]]@(0.3, -0.5, -0.8),
    [double[]]@(0.2, 0.9, -0.3)
)

function Test-Chain([double[]]$Vraw, [string]$name) {
    $detRaw = Det3 $Vraw

    # ---- camOrthoSide(colMode=true) replica (main.cpp 2949-2978) ----
    $m = [double[]]$Vraw.Clone()
    $ln = New-Object double[] 3
    $ok = $true
    for ($k = 0; $k -lt 3; $k++) {
        $n2 = 0.0
        for ($jj = 0; $jj -lt 3; $jj++) { $e = $m[$jj*3+$k]; $n2 += $e*$e }
        if ($n2 -lt 0.0025 -or $n2 -gt 400.0) { $ok = $false; break }
        $ln[$k] = [Math]::Sqrt($n2)
    }
    if (-not $ok) { Write-Output ("{0}: colMode LENGTH FAIL" -f $name); return }
    for ($i = 0; $i -lt 3; $i++) {
        for ($k = $i + 1; $k -lt 3; $k++) {
            $d = 0.0
            for ($jj = 0; $jj -lt 3; $jj++) { $d += $m[$jj*3+$i] * $m[$jj*3+$k] }
            if ([Math]::Abs($d) -gt 1e-3 * $ln[$i] * $ln[$k]) { $ok = $false }
        }
    }
    if (-not $ok) { Write-Output ("{0}: colMode ORTHO FAIL" -f $name); return }
    for ($i = 0; $i -lt 3; $i++) { for ($jj = 0; $jj -lt 3; $jj++) { $m[$jj*3+$i] /= $ln[$i] } }

    $flipped = $false
    if ((Det3 $m) -lt 0.0) {
        for ($jj = 0; $jj -lt 3; $jj++) { $m[$jj*3+2] = -$m[$jj*3+2] }  # flip third LINE (col 2)
        $flipped = $true
    }
    $C = $m

    # ---- NEW: chain = D^f . C^T . D  (colMode branch) ----
    $chain = New-Object double[] 9
    for ($i = 0; $i -lt 3; $i++) { for ($jj = 0; $jj -lt 3; $jj++) { $chain[$i*3+$jj] = $C[$jj*3+$i] } }  # C^T
    if ($flipped) { for ($jj = 0; $jj -lt 3; $jj++) { $chain[2*3+$jj] = -$chain[2*3+$jj] } }               # left  D
    for ($i = 0; $i -lt 3; $i++) { $chain[$i*3+2] = -$chain[$i*3+2] }                                     # right D

    $qN = CamQuat $chain
    # ---- OLD: quat(C), camconj=1 (negate xyz) ----
    $qO = CamQuat $C
    if ($qO) { $qO[0] = -$qO[0]; $qO[1] = -$qO[1]; $qO[2] = -$qO[2] }

    if (-not $qN) { Write-Output ("{0}: NEW quat FAIL (degenerate chain)" -f $name); return }
    if (-not $qO) { Write-Output ("{0}: OLD quat FAIL" -f $name); return }

    $errN = 0.0
    $errO = 0.0
    $first = $true
    foreach ($v in $vecs) {
        $dv = New-Object double[] 3          # D . v  (element-wise: comma literals mis-parse arithmetic)
        $dv[0] = $v[0]
        $dv[1] = $v[1]
        $dv[2] = -$v[2]
        $exp = New-Object double[] 3
        for ($i = 0; $i -lt 3; $i++) {           # V^T . dv  (row-major V)
            $exp[$i] = $Vraw[0*3+$i]*$dv[0] + $Vraw[1*3+$i]*$dv[1] + $Vraw[2*3+$i]*$dv[2]
        }
        $gN = RotQ $qN $v
        $gO = RotQ $qO $v
        for ($i = 0; $i -lt 3; $i++) {
            $eN = [Math]::Abs($gN[$i] - $exp[$i]); if ($eN -gt $errN) { $errN = $eN }
            $eO = [Math]::Abs($gO[$i] - $exp[$i]); if ($eO -gt $errO) { $errO = $eO }
        }
        if ($first) {
            $first = $false
            Write-Output ("{0}: center-ray d_world = ({1:F4}, {2:F4}, {3:F4})  [expect SE-down: +X east, -Y south, -Z down]" -f $name, $exp[0], $exp[1], $exp[2])
        }
    }
    $chainDet = Det3 $chain
    if (-not $flipped) {
        # raw det=+1 => map V^T.D is improper, NOT quat-representable;
        # the publish site must GUARD (det(chain)<0 -> skip). That is correct.
        Write-Output ("{0}: det(raw)={1:F6} flipped=False det(chain)={2:F6} | guard expected -> {3}" -f `
            $name, $detRaw, $chainDet, $(if ($chainDet -lt -0.5) { "GUARD-OK" } else { "CHECK" }))
    } else {
        Write-Output ("{0}: det(raw)={1:F6} flipped=True det(chain)={2:F6} | NEW maxErr={3:E2} OLD maxErr={4:E2}  {5}" -f `
            $name, $detRaw, $chainDet, $errN, $errO, $(if ($errN -lt 1e-5 -and $errO -gt 1e-3) { "PASS" } else { "CHECK" }))
    }
}

# =========================== tests ===================================

Write-Output "=== P (game projection, ev=24082) ==="
Write-Output ("A={0} B={1} C={2} D={3}" -f $PA, $PB, $PC, $PD)
$thY = 1.0 / $PB
$fovY = 2.0 * [Math]::Atan($thY) * 180.0 / [Math]::PI
$fovX = 2.0 * [Math]::Atan(1.0 / $PA) * 180.0 / [Math]::PI
$near = -$PD / $PC
$far = $PC * $near / ($PC - 1.0)
Write-Output ("tanHalfY={0} fovY={1:F6} deg  fovX={2:F4} deg  aspect={3:F6}" -f $thY, $fovY, $fovX, ($PB / $PA))
Write-Output ("near={0:F6} far={1:F4}" -f $near, $far)
Write-Output ("ini: ssr.fov={0} ssr.near={1} ssr.far={2}" -f $fovY.ToString("F4"), $near.ToString("F4"), $far.ToString("F4"))

Write-Output "=== chain tests ==="
Test-Chain $V1 "V1-real(RD ev24082)"

# V2 = V1 . Rz(37deg): still det=-1, different orientation
$c37 = [Math]::Cos(37.0 * [Math]::PI / 180.0)
$s37 = [Math]::Sin(37.0 * [Math]::PI / 180.0)
$ns37 = -$s37
$Rz = ,([double[]]@($c37, $ns37, 0.0)) + ,([double[]]@($s37, $c37, 0.0)) + ,([double[]]@(0.0, 0.0, 1.0))
$Rz9 = New-Object double[] 9
$idx = 0
foreach ($row in $Rz) { for ($jj = 0; $jj -lt 3; $jj++) { $Rz9[$idx] = $row[$jj]; $idx++ } }
$V2 = Mul3 $V1 $Rz9
Test-Chain $V2 "V2=V1*Rz(37) (improper)"

# V3 = V1 . D : det=+1 -> camBlockOk does NOT flip (tests the other branch)
$Dm = ,([double[]]@(1.0, 0.0, 0.0)) + ,([double[]]@(0.0, 1.0, 0.0)) + ,([double[]]@(0.0, 0.0, -1.0))
$D9 = New-Object double[] 9
$idx = 0
foreach ($row in $Dm) { for ($jj = 0; $jj -lt 3; $jj++) { $D9[$idx] = $row[$jj]; $idx++ } }
$V3 = Mul3 $V1 $D9
Test-Chain $V3 "V3=V1*D (proper, no-flip branch)"

Write-Output "=== done ==="
