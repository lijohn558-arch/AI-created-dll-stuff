param([string]$Path)
# px_hitmiss.ps1  -- reader for ssr.debug=2 screenshots (green=hit / red=miss).
#
# WHAT debug=2 does (ssr.frag:351):   hit ? vec4(0,1,0,1) : vec4(1,0,0,1)
# It answers "is wSsr zero because the ray MISSED, or because something else?"
# which is the split needed after sec14.31 sec8 cleared `gb`:
#     add = strength * gb * bf^2 * max(baseRGB - refl_ssr, 0)   , gated by hit
#   mostly MISS  -> hit is the gate -> that is the queue-#2 B problem (miss -> real
#                   cubemap), and issue A only ever applies on the hit band
#   mostly HIT   -> hit is innocent -> the killer is bf^2 or max(baseRGB-refl_ssr,0)
#                   -> then (and only then) add a debug=9 three-factor readout
#
# WHY G-R AND NOT ABSOLUTE VALUE: the shader's output is 585, and the screen shows
#     mix(585, 588, w)  with 585 sharing ~90% (docs/02 sec14.29)
# so a "green" pixel reads  ~0.9*G + 0.1*sceneG  and  ~0.1*sceneR.
#   => green: G-R ~ +0.9    red: R-G ~ +0.9    neutral scene: G-R ~ 0
# Absolute thresholds on G or R alone would classify the SCENE as a hit.
#
# Boxes are byte-for-byte the same pose-FIXED ones as px_v1det.ps1 so the two
# readers describe the same rectangles; always run px_v1det -Mark first to check
# they are still on water (docs/02 sec14.31 sec6).
#
# ASCII-only; CRLF no BOM (tools convention).  exit 0 = ok, 2 = bad input.
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$bx = @(
    'far',    200,  210, 1700,  380,
    'midR',  1050,  430, 1850,  700,
    'nearL',   60,  760,  420, 1070,
    'nearR', 1450,  760, 1900, 1070,
    'ALL',       0,    0, 1920, 1080
)
$nb = [int]($bx.Count / 5)

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

# G-R >= +Tol  => hit (green).  R-G >= +Tol => miss (red).  else => scene / not water.
# 0.15 is far below the ~0.9 a pure 90%-mixed channel pair produces, and far above the
# G-R jitter of a neutral grey/rock pixel.
$Tol = 0.15

$hit  = New-Object long[] $nb
$miss = New-Object long[] $nb
$oth  = New-Object long[] $nb
$GW = 64; $GH = 24
$gHit = New-Object 'long[,]' $GH, $GW
$gTot = New-Object 'long[,]' $GH, $GW

for ($y = 0; $y -lt $H; $y++) {
    $row = $y * $W * 4
    $gy = [int][Math]::Floor($y * $GH / $H)
    for ($x = 0; $x -lt $W; $x++) {
        $i = $row + $x * 4
        $b = [int]$px[$i]; $g = [int]$px[$i + 1]; $r = [int]$px[$i + 2]
        $cls = 0            # 0 = other, 1 = hit, 2 = miss
        $d = ($g - $r) / 255.0
        if ($d -ge $Tol) { $cls = 1 } elseif ($d -le -$Tol) { $cls = 2 }
        if ($cls -ne 0) {
            $gxc = [int][Math]::Floor($x * $GW / $W)
            $gTot[$gy, $gxc]++
            if ($cls -eq 1) { $gHit[$gy, $gxc]++ }
        }
        for ($bi = 0; $bi -lt $nb; $bi++) {
            $o = $bi * 5
            if ($x -ge $bx[$o+1] -and $x -lt $bx[$o+3] -and $y -ge $bx[$o+2] -and $y -lt $bx[$o+4]) {
                if ($cls -eq 1) { $hit[$bi]++ }
                elseif ($cls -eq 2) { $miss[$bi]++ }
                else { $oth[$bi]++ }
                break
            }
        }
    }
}

Write-Output ""
Write-Output ("hit/miss per box (classify by G-R, tol +/-" + $Tol + "; scene pixels fall in 'other'):")
$hdr = 'box     ' + 'n'.PadLeft(9) + 'water'.PadLeft(9) + 'water%'.PadLeft(9) +
       'hit%'.PadLeft(9) + 'miss%'.PadLeft(9) + 'other%'.PadLeft(9)
Write-Output $hdr
for ($bi = 0; $bi -lt $nb; $bi++) {
    # box pixel count from the stride-5 coordinates (name,x1,y1,x2,y2)
    $n = ([long]([int]$bx[$bi*5+3] - [int]$bx[$bi*5+1])) * ([long]([int]$bx[$bi*5+4] - [int]$bx[$bi*5+2]))
    $w = $hit[$bi] + $miss[$bi]
    $row = ([string]$bx[$bi*5]).PadRight(7) + $n.ToString().PadLeft(9)
    $row += $w.ToString().PadLeft(9)
    $row += ("{0,8:N2}%" -f (100.0 * $w / $n)).PadLeft(9)
    if ($w -gt 0) {
        $row += ("{0,8:N2}%" -f (100.0 * $hit[$bi] / $w)).PadLeft(9)
        $row += ("{0,8:N2}%" -f (100.0 * $miss[$bi] / $w)).PadLeft(9)
    } else {
        $row += "       -".PadLeft(9) + "       -".PadLeft(9)
    }
    $row += ("{0,8:N2}%" -f (100.0 * $oth[$bi] / $n)).PadLeft(9)
    Write-Output $row
}
Write-Output ""
Write-Output "hit% / miss% are OF THE CLASSIFIED WATER PIXELS ONLY - other% is scene / non-water."
Write-Output "CAVEAT: boxes are pose-FIXED (framed for v0.18.14).  Run px_v1det -Mark on the shot"
Write-Output "        first, or these rectangles may be measuring rock instead of water."

Write-Output ""
Write-Output "hit/miss map (H=hit, m=miss, '='=mixed, .=too few water pixels):"
for ($gy = 0; $gy -lt $GH; $gy++) {
    $line = ''
    for ($gx = 0; $gx -lt $GW; $gx++) {
        $t = $gTot[$gy, $gx]
        if ($t -lt 20) { $line += '.'; continue }
        $f = [double]$gHit[$gy, $gx] / $t
        if ($f -ge 0.60) { $line += 'H' }
        elseif ($f -le 0.40) { $line += 'm' }
        else { $line += '=' }
    }
    Write-Output $line
}

Write-Output ""
$wAll = $hit[$nb-1] + $miss[$nb-1]
if ($wAll -le 0) {
    Write-Output "VERDICT: no classified water pixels at all - wrong debug mode, or not a debug=2 shot."
    exit 2
}
$hf = 100.0 * $hit[$nb-1] / $wAll
if ($hf -ge 60.0) {
    Write-Output ("VERDICT: " + ("{0:N1}" -f $hf) + "% HIT -> hit is innocent; killer is bf^2 or max(baseRGB-refl_ssr,0).")
    Write-Output "         Next step: add debug=9 three-factor readout (R=gb / G=wSsr / B=max())."
} elseif ($hf -le 40.0) {
    Write-Output ("VERDICT: " + ("{0:N1}" -f (100.0 - $hf)) + "% MISS -> wSsr=0 is the gate.  That is queue #2's B problem")
    Write-Output "         (miss -> real cubemap); issue A only ever applies on the thin hit band."
} else {
    Write-Output ("VERDICT: mixed (" + ("{0:N1}" -f $hf) + "% hit) - read the map, not the average.")
}
exit 0
