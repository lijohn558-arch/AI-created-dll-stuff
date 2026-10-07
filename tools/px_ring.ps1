# px_ring.ps1 -- SSR「物体外一圈绿边」像素取证 (docs/02 §14.28.1 / §14.30)
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\px_ring.ps1
#   powershell ... -File tools\px_ring.ps1 -Dir "C:\...\docs\analysis" -Files "a.png","b.png"
#
# 判据取**与机位无关的局部量** (三张图不是同一存档加载, 总面积只能当参考):
#   环 = 长度 1..24px 的绿色 run, 且紧邻一侧有 >=25px 的未涂色物体 run;
#   「环核」= 其中 3..6px 的那批 (ssr.smooth=4 的中心差分半径)。
# 分类: G = G>170 且 R<130 且 B<130;  R = R>170 且 G<130 且 B<130;  O = 其余 (物体/未涂色)。
#
# 三段输出:
#   1. 面积计数            -- 只当参考 (机位不同), 绿/水面 才是归一后的命中率
#   2. 抽行环统计          -- 中位数/主峰: 中位数 == ssr.smooth => 归因坐实
#   3. 逐行 y 分带 + 环核  -- 环长在哪些剪影上 (人物 / 底部礁石 / 贴岸带)

param(
    [string]$Dir = "C:\Users\joker\skyrim-vulkan\docs\analysis",
    [string[]]$Files = @(
        "Screenshot_v0.18.12 ssr.debug=2.png",
        "Screenshot_v0.18.12 ssr.debug=2 ssr.fov=58.7155 .png",
        "Screenshot_v0.18.12 ssr.debug=2 ssr.steps=128.png"
    )
)

Add-Type -AssemblyName System.Drawing

function Load-Pixels([string]$path) {
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

function Class-Of($p, [int]$i) {
    $bl = $p.b[$i]; $gr = $p.b[$i + 1]; $rd = $p.b[$i + 2]
    if ($gr -gt 170 -and $rd -lt 130 -and $bl -lt 130) { return "G" }
    if ($rd -gt 170 -and $gr -lt 130 -and $bl -lt 130) { return "R" }
    return "O"
}

# 取一行的 run 表: @(@{c=cls; s=start; e=end}, ...)  (cls 只有 G/R/O)
function Row-Runs($p, [int]$y) {
    $base = $y * $p.stride
    $runs = @()
    $cur = $null; $st = 0
    for ($x = 0; $x -lt $p.w; $x++) {
        $c = Class-Of $p ($base + $x * 3)
        if ($c -ne $cur) {
            if ($cur -ne $null) { $runs += , @{ c = $cur; s = $st; e = ($x - 1) } }
            $cur = $c; $st = $x
        }
    }
    $runs += , @{ c = $cur; s = $st; e = ($p.w - 1) }
    return $runs
}

$bands = @()
$bands += , @(400, 599)
$bands += , @(600, 799)
$bands += , @(800, 999)
$bands += , @(1000, 1079)

Write-Output ("==== 0. files: {0}  dir: {1}" -f $Files.Count, $Dir)

# ---- 1. 面积计数 (参考值) ----
Write-Output "==== 1. area counts -- INDICATIVE ONLY (screenshots come from different save loads) ===="
foreach ($f in $Files) {
    $p = Load-Pixels (Join-Path $Dir $f)
    $g = 0; $r = 0
    for ($y = 0; $y -lt $p.h; $y++) {
        $row = $y * $p.stride
        for ($x = 0; $x -lt $p.w; $x++) {
            $c = Class-Of $p ($row + $x * 3)
            if ($c -eq "G") { $g++ } elseif ($c -eq "R") { $r++ }
        }
    }
    $tot = $p.w * $p.h; $wat = $g + $r
    Write-Output ("{0}`n    green={1} ({2:P2})  red={3} ({4:P2})  water(g+r)={5} ({6:P2})  green/water={7:P2}" -f
        $f, $g, ($g / $tot), $r, ($r / $tot), $wat, ($wat / $tot), $(if ($wat -gt 0) { $g / $wat } else { 0 }))
}

# ---- 2. 抽行环统计: 中位数/主峰 == ssr.smooth ? ----
Write-Output "==== 2. RING width over sampled rows (y = 400..1060 step 10) ===="
Write-Output "    ring = green run 1..24px touching an object run >=25px"
foreach ($f in $Files) {
    $p = Load-Pixels (Join-Path $Dir $f)
    $lens = @()
    for ($y = 400; $y -le 1060; $y += 10) {
        if ($y -ge $p.h) { break }
        $runs = Row-Runs $p $y
        for ($k = 1; $k -lt ($runs.Count - 1); $k++) {
            $r0 = $runs[$k]
            if ($r0.c -ne "G") { continue }
            $len = $r0.e - $r0.s + 1
            if ($len -lt 1 -or $len -gt 24) { continue }
            $L = $runs[$k - 1]; $R = $runs[$k + 1]
            $ok = ($L.c -eq "O" -and ($L.e - $L.s + 1) -ge 25) -or ($R.c -eq "O" -and ($R.e - $R.s + 1) -ge 25)
            if ($ok) { $lens += $len }
        }
    }
    if ($lens.Count -eq 0) {
        Write-Output ("{0}`n    NO ring found" -f $f)
    } else {
        $srt = $lens | Sort-Object
        $med = $srt[[int]($srt.Count / 2)]
        $hist = $lens | Group-Object | Sort-Object { [int]$_.Name } | ForEach-Object { "{0}px:{1}" -f $_.Name, $_.Count }
        Write-Output ("{0}`n    hits={1} min={2} median={3} max={4}`n    hist {5}" -f
            $f, $lens.Count, $srt[0], $med, $srt[$srt.Count - 1], ($hist -join "  "))
    }
}

# ---- 3. 逐行 y 分带 + 环核 (3..6px) ----
Write-Output "==== 3. RING by y-band, every row y = 400..1079;  ringcore = 3..6px ===="
foreach ($f in $Files) {
    $p = Load-Pixels (Join-Path $Dir $f)
    $all = @(0, 0, 0, 0); $core = @(0, 0, 0, 0); $mx = @(0, 0, 0, 0)
    for ($y = 400; $y -le 1079; $y++) {
        if ($y -ge $p.h) { break }
        $bi = -1
        for ($t = 0; $t -lt $bands.Count; $t++) {
            if ($y -ge $bands[$t][0] -and $y -le $bands[$t][1]) { $bi = $t; break }
        }
        if ($bi -lt 0) { continue }
        $runs = Row-Runs $p $y
        for ($k = 1; $k -lt ($runs.Count - 1); $k++) {
            $r0 = $runs[$k]
            if ($r0.c -ne "G") { continue }
            $len = $r0.e - $r0.s + 1
            if ($len -lt 1 -or $len -gt 24) { continue }
            $L = $runs[$k - 1]; $R = $runs[$k + 1]
            $ok = ($L.c -eq "O" -and ($L.e - $L.s + 1) -ge 25) -or ($R.c -eq "O" -and ($R.e - $R.s + 1) -ge 25)
            if ($ok) {
                $all[$bi]++
                if ($len -ge 3 -and $len -le 6) { $core[$bi]++ }
                if ($len -gt $mx[$bi]) { $mx[$bi] = $len }
            }
        }
    }
    $parts = @(); $ta = 0; $tc = 0
    for ($t = 0; $t -lt $bands.Count; $t++) {
        $parts += ("y{0}-{1}: {2} (core {3}, wmax {4})" -f $bands[$t][0], $bands[$t][1], $all[$t], $core[$t], $mx[$t])
        $ta += $all[$t]; $tc += $core[$t]
    }
    $share = $(if ($ta -gt 0) { $tc / $ta } else { 0 })
    Write-Output ("{0}`n    {1}`n    total={2} core={3} ({4:P0})" -f $f, ($parts -join "  |  "), $ta, $tc, $share)
}
Write-Output "done"
