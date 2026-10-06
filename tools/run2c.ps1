$ErrorActionPreference = "Stop"
$logfile = "C:\Users\joker\AppData\Local\ModOrganizer\Skyrim Special Edition fsr\overwrite\SKSE\Plugins\poc-presenter.log"
$base = "C:\Users\joker\AppData\Local\Temp\opencode"
$out = New-Object System.Collections.Generic.List[string]

if (-not (Test-Path $logfile)) { $out.Add("LOG NOT FOUND: $logfile") }
$fi = Get-Item $logfile
$lines = [System.IO.File]::ReadAllLines($logfile, [System.Text.Encoding]::UTF8)
$out.Add("bytes=" + $fi.Length + "  mtime=" + $fi.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss") + "  total_lines=" + $lines.Count)

# 日志是**跨会话追加**的 (实测: v0.14.0 会话与 v0.15.0 会话同在一个文件)。
# 只判**最后一个会话** —— 从最后一行 banner 起切片, 否则旧会话的 ini/2a/2b 计数会污染判读
# (首版就是这么把 "banner 版本不对" 和 "ssr.sentinel 还开着" 判成 FAIL 的)。
$bannerIdx = -1
for ($bi = 0; $bi -lt $lines.Count; $bi++) { if ($lines[$bi].Contains("==== poc-presenter ")) { $bannerIdx = $bi } }
$sessionN = 0
foreach ($x in $lines) { if ($x.Contains("==== poc-presenter ")) { $sessionN++ } }
if ($bannerIdx -ge 0 -and $bannerIdx -gt 0) { $lines = $lines[$bannerIdx..($lines.Count - 1)] }
$out.Add("sessions=" + $sessionN + "  analyzing=LAST (from line " + ($bannerIdx + 1) + ", " + $lines.Count + " lines)")
$out.Add("")

# 踩坑: PowerShell 变量大小写不敏感, foreach 的循环变量绝不能与数组同名(只差大小写) --
# 否则 foreach ($l in $L) 会把 $L 就地覆盖成最后一行, 之后所有统计恒 0。统一用 $x。

function Cnt([string]$sub) {
    $k = 0
    foreach ($x in $script:lines) { if ($x.Contains($sub)) { $k++ } }
    return $k
}
function Pick([string]$sub, [int]$m) {
    $r = New-Object System.Collections.Generic.List[string]
    foreach ($x in $script:lines) { if ($x.Contains($sub)) { $r.Add($x); if ($r.Count -ge $m) { break } } }
    return $r
}
# 正则版计数 (槽K(名字) 这种带变量的判据要用 -match)
function CntRe([string]$re) {
    $k = 0
    foreach ($x in $script:lines) { if ($x -match $re) { $k++ } }
    return $k
}
# 同一行必须同时含两段 (归因字段只在**不一致**的行里才算数 —— 一致时"同刻==VK"是必然成立的)
function CntBoth([string]$a, [string]$b) {
    $k = 0
    foreach ($x in $script:lines) { if ($x.Contains($a) -and $x.Contains($b)) { $k++ } }
    return $k
}

$out.Add("==== #0 banner 版本 (判读第 0 步) ====")
$bn = ""
$ver = ""
foreach ($x in $lines) { if ($x.Contains("==== poc-presenter ")) { $bn = $x; break } }
if ($bn -eq "") { $out.Add("  [FAIL] 没有 banner 行") }
else {
    $m2 = [regex]::Match($bn, "poc-presenter (v[0-9.]+)")
    if ($m2.Success) { $ver = $m2.Groups[1].Value }
    $out.Add("  expect v0.18.3: " + $(if ($ver -eq "v0.18.3") { "OK" } else { "MISMATCH -> " + $ver }))
    $out.Add("  banner 含 ssr.shared: " + $(if ($bn.Contains("ssr.shared")) { "OK" } else { "FAIL (banner 没升)" }))
    $out.Add("  banner 含 Step2c-β: " + $(if ($bn.Contains("Step2c-β")) { "OK" } else { "FAIL (banner 没含 2c-β)" }))
    $out.Add("  banner 含 ssr.sentinel: " + $(if ($bn.Contains("ssr.sentinel")) { "OK" } else { "FAIL" }))
    $out.Add("  banner 含 ssr.vkout: " + $(if ($bn.Contains("ssr.vkout")) { "OK" } else { "FAIL (banner 没升到 2d)" }))
}
$out.Add("")

$out.Add("==== #1/#2/#3/#4/#5 挂载与镜像创建 ====")
$n_ssr  = Cnt "ini ssr=1 "          # "ini ssr=1 → 挂 ctx 槽..." (不会误配 sentinel)
$out.Add("  ini ssr=1 = " + $n_ssr + "   [应为1]")
$n_shr  = Cnt "ini ssr.shared=1"
$out.Add("  ini ssr.shared=1 = " + $n_shr + "   [应为1]")
$n_sen  = Cnt "ini ssr.sentinel=1 "
$out.Add("  ini ssr.sentinel=1 = " + $n_sen + "   [应为0, 哨兵关着]")
$hookN = Cnt "ctx槽33/50/47 已挂"
$out.Add("  ctx槽33/50/47 已挂 = " + $hookN + "   [应为1]")
$n_c = Cnt "[2c] 色324 SHARED 镜像 OK"
$n_d = Cnt "[2c] 深度520 SHARED 镜像 OK"
$out.Add("  [2c] 色324 SHARED 镜像 OK = " + $n_c + "   [#2 应为1]")
$out.Add("  [2c] 深度520 SHARED 镜像 OK = " + $n_d + "   [#3 应为1]")
$r2 = Cnt "R2 风险点"
$out.Add("  <--- #4 R2 风险点失败行 = " + $r2 + "   [0=R2全过; >0=R2半过]")
foreach ($x in (Pick "R2 风险点" 6)) { $out.Add("      " + $x) }
$n_ready = Cnt "[2c] 共享入向就绪"
$out.Add("  [2c] 共享入向就绪 = " + $n_ready + "   [#5 应为1]")
foreach ($x in (Pick "[2c] 共享入向就绪" 2)) { $out.Add("      " + $x) }
$out.Add("  [2a] 场景色快照 324 = " + (Cnt "[2a] 场景色快照") + "   [1]")
$out.Add("  [2a] 深度快照 520    = " + (Cnt "[2a] 深度快照") + "   [1]")
$out.Add("")

$out.Add("==== #6 [2c] 入向拷贝 ====")
$n6 = Cnt "[2c] 入向拷贝#"
$out.Add("  日志条数 = $n6   [前8条+每128条, 实机应 >0]")
foreach ($x in (Pick "[2c] 入向拷贝#" 3)) { $out.Add("      " + $x) }
$last6 = ""
foreach ($x in $lines) { if ($x.Contains("[2c] 入向拷贝#")) { $last6 = $x } }
if ($last6 -ne "") { $out.Add("      LAST: " + $last6) }
$out.Add("")

$out.Add("==== #7 [2c读回] 自校验 (核心判据) ====")
$n7 = Cnt "[2c读回"
$nOk  = Cnt "≠基线"
$nBad = Cnt "=基线"
$out.Add("  [2c读回 行数 = $n7   [前3次+每600次]")
$out.Add("  含 ≠基线 = " + $nOk + "    [>0 且 == 行数 => 拷贝真落地]")
$out.Add("  含 =基线 = " + $nBad + "    [>0 => CopyResource 静默失败! desc 不匹配]")
foreach ($x in (Pick "[2c读回" 8)) { $out.Add("      " + $x) }
$out.Add("")

$out.Add("==== #8/#9 汇总与排队 ====")
$lastSum = ""
foreach ($x in $lines) { if ($x.Contains("入向=")) { $lastSum = $x } }
$out.Add("  LAST summary: " + $(if ($lastSum -ne "") { $lastSum } else { "(没有含 入向= 的汇总行!)" }))
$inN = 0
$m3 = [regex]::Match($lastSum, "入向=(\d+)")
if ($m3.Success) { $inN = [int]$m3.Groups[1].Value }
$out.Add("  入向累计 = $inN   [#8 应持续增长, 实机明显 >0]")
$m4 = [regex]::Match($lastSum, "COPY=(\d+)")
$out.Add("  COPY= 末值 = " + $(if ($m4.Success) { $m4.Groups[1].Value } else { "?" }) + "   [应为3]")
$m5 = [regex]::Match($lastSum, "哨兵=(\d+)")
$out.Add("  哨兵= 末值 = " + $(if ($m5.Success) { $m5.Groups[1].Value } else { "?" }) + "   [应为0]")
$out.Add("  特征B(换绑) 总数 = " + (Cnt "特征B(换绑)") + "   [实机每帧1次]")
$nq = Cnt "[2c入向已排队]"
$out.Add("  [2c入向已排队] = " + $nq + "   [#9 实机应 >0]")
$out.Add("  [2b哨兵已排队] = " + (Cnt "[2b哨兵已排队]") + "   [sentinel=0 时该标注仍会出现, 只是不执行]")
$nSen = Cnt "[2b] 哨兵#"
$out.Add("  [2b] 哨兵 执行 = " + $nSen + "   [应为0]")
$out.Add("")

$out.Add("==== #10/#11 副作用面 ====")
$nAnom = Cnt "[异常]"
$out.Add("  [异常] = $nAnom   [#11 应为0]")
foreach ($x in (Pick "[异常]" 5)) { $out.Add("      " + $x) }
$perf = New-Object System.Collections.Generic.List[string]
foreach ($x in $lines) { if ($x.Contains("ms/帧")) { $perf.Add($x) } }
$out.Add("  PoC-B 性能 (取末5条, Step1 基线 0.87):")
if ($perf.Count -eq 0) { $out.Add("      (无) <-- PoC-B 没跑起来?") }
else { foreach ($x in ($perf | Select-Object -Last 5)) { $out.Add("      " + $x) } }
$out.Add("")

$out.Add("==== β1..β6 Step 2c-β (VK 导入 + 交叉校验) ====")
$nB1 = Cnt "[2c-β] VK 导入 OK"
$out.Add("  β2 [2c-β] VK 导入 OK = " + $nB1 + "   [#2 应为1]")
foreach ($x in (Pick "[2c-β] VK 导入 OK" 3)) { $out.Add("      " + $x) }
$nB1f = Cnt "VK 交叉校验停用"
if ($nB1f -gt 0) { foreach ($x in (Pick "VK 交叉校验停用" 4)) { $out.Add("   [归因] " + $x) } }
$nB1b = Cnt "跳过 VK 导入"
if ($nB1b -gt 0) { foreach ($x in (Pick "跳过 VK 导入" 3)) { $out.Add("   [跳过] " + $x) } }
$nB1s = Cnt "读回提交失败"
if ($nB1s -gt 0) { foreach ($x in (Pick "读回提交失败" 3)) { $out.Add("   [失败] " + $x) } }
# --- β2a 导入归因矩阵 (v0.16.1) ---
$nMat = Cnt "导入矩阵"
if ($nMat -gt 0) {
    foreach ($x in (Pick "导入矩阵" 4)) { $out.Add("   [矩阵] " + $x) }
}
$nPa = Cnt "归因探测A"
$nPb = Cnt "归因探测B"
if ($nPa -gt 0) { foreach ($x in (Pick "归因探测A" 2)) { $out.Add("   [探针A] " + $x + "   [A=RGBA8@1920x1080 只换格式, OK 说明尺寸才是问题]") } }
if ($nPb -gt 0) { foreach ($x in (Pick "归因探测B" 2)) { $out.Add("   [探针B] " + $x + "   [B=RGBA16F@512x512 只换尺寸, OK 说明格式才是问题]") } }
$nConcl = Cnt "归因结论:"
if ($nConcl -gt 0) { foreach ($x in (Pick "归因结论:" 3)) { $out.Add("   [结论] " + $x) } }
$nB2 = Cnt "[2c-β] 交叉校验#"
$nBok  = Cnt "**一致✓**"
$nBbad = Cnt "**不一致✗**"
$out.Add("  β3 [2c-β] 交叉校验 行数 = $nB2   [v0.16.6 单图常驻: 首次建图那 1 次机会不比, 之后每次机会都出一行 => 应约为 #7-1]")
$out.Add("      一致 = $nBok    不一致 = $nBbad   [判据: **全部一致**(单参数已定案), 至少 ≥4 次一致才收口]")
foreach ($x in (Pick "[2c-β] 交叉校验#" 8)) { $out.Add("      " + $x) }
$g1 = CntBoth "**不一致✗**" "=VK✓时序差"
$g2 = CntBoth "**不一致✗**" "=VK✓差一帧"
$g3 = CntBoth "**不一致✗**" "=D3D11✓行距归因"
$g0 = CntBoth "**不一致✗**" "镜像帧内被改"
$out.Add("      [v0.16.3 归因(只统计不一致的行)] 同刻=VK(时序) $g1 | 前帧=VK(差一帧) $g2 | 按D3D11行距=D3D11(行距) $g3 | 镜像帧内被改 $g0")
if (($g1 + $g2 + $g3 + $g0) -eq 0) {
    if ($nBbad -eq 0) { $out.Add("      [v0.16.3 归因] 没有不一致的行 => 不需要归因 (全部一致)") }
    else { $out.Add("      [v0.16.3 归因] 三项全 0 => 时序/行距/镜像被改全排除, 落在归因1: 布局/字节序") }
} else {
    $out.Add("      [v0.16.3 归因] 有命中项 => 按命中项改 (时序→查特征B后谁写镜像; 差一帧→读回挪到特征B后; 行距→统一 pitch)")
}
$nSlotBuild = Cnt "建图+导入OK"
$nSlotSkip  = Cnt ") vkCreateImage ="
$nSlotSkip2 = Cnt "次全失败"
$nDLost     = Cnt "DEVICE_LOST"
$nPocbDL    = CntBoth "PoC-B 失败:" "code=-4"   # device lost 是 **PoC-B 的提交**报出来的 (2c-β 那次可能成功)
$nPocbFail  = Cnt "PoC-B 失败:"
$out.Add("  [2c-β 单图] 建图成功行 = $nSlotBuild   建图失败换槽行 = $nSlotSkip   导入失败换槽行 = $nSlotSkip2")
$out.Add("      DEVICE_LOST(2c-β 自己) = $nDLost   PoC-B 失败行 = $nPocbFail (其中 code=-4 设备丢 = $nPocbDL)")
if ($nPocbFail -gt 0) { foreach ($x in (Pick "PoC-B 失败:" 3)) { $out.Add("      " + $x) } }
foreach ($x in (Pick "建图+导入OK" 4)) { $out.Add("      " + $x) }
$slotNames = @("D3D11句柄", "LINEAR", "全程GENERAL", "对照原样")
$anySlotOk = 0
$out.Add("  [各槽结果]  (槽K(名字) 在 **一致✓** 的后面, 按整行同时含两段来数)")
foreach ($s in $slotNames) {
    $ok = 0; $bad = 0
    $esc = [regex]::Escape($s)
    foreach ($x in $script:lines) {
        if ($x -match ("槽\d\(" + $esc + "\)")) {
            if ($x.Contains("**一致✓**")) { $ok++ }
            elseif ($x.Contains("**不一致✗**")) { $bad++ }
        }
    }
    if (($ok + $bad) -gt 0) {
        $out.Add("      槽[" + $s + "]: 一致 $ok / 不一致 $bad" + $(if ($ok -gt 0) { "   <== 命中 (该参数与 D3D11 字节视图一致)" } else { "" }))
        if ($ok -gt 0) { $anySlotOk = $anySlotOk + $ok }
    } else {
        $out.Add("      槽[" + $s + "]: 还没跑到 (v0.16.6 起只剩 D3D11句柄 一个槽, 其余三槽已删)")
    }
}
if ($anySlotOk -gt 0) { $out.Add("      => 命中: 该槽参数即 2c 收口方案 (v0.16.6 已把它定为唯一参数, 看 [各槽结果] 的样本数)") }
elseif ($nB2 -gt 0) { $out.Add("      => 已跑到的槽全不一致: 看下方 [v0.16.3 归因] 与失败行, 换参数继续") }
$nRetry = Cnt "[2c]   重试"
$out.Add("  β4 R2 归因重试行 = $nRetry   [0=深度首次就建成了(意外之喜); 1..2=做了归因]")
foreach ($x in (Pick "[2c]   重试" 4)) { $out.Add("      " + $x) }
$n3 = Cnt "病因是格式"
$n2b = Cnt "病因是 BindFlags"
if (($nRetry -gt 0)) { $out.Add("      归因结论: 病因是格式 x$n3 / 病因是 BindFlags x$n2b") }
$nB3 = Cnt "三种 BindFlags 全失败"
if ($nB3 -gt 0) { foreach ($x in (Pick "三种 BindFlags 全失败" 2)) { $out.Add("      [结论] " + $x) } }
$out.Add("")

$out.Add("==== #12 Step 2d-1 出向回写 (v0.18.3) ====")
$nVkoIni = Cnt "ini ssr.vkout=1 → 2d 出向回写"
$out.Add("  ini ssr.vkout 读到 = $nVkoIni   [应为1; 0 => ini 没写 ssr.vkout=1, 本轮回退到纯 2c 形态]")
foreach ($x in (Pick "ini ssr.vkout" 3)) { $out.Add("      " + $x) }
$nOutImg = Cnt "[2d] 出向镜像 OK"
$out.Add("  [2d] 出向镜像 OK = $nOutImg   [应为1]")
foreach ($x in (Pick "[2d] 出向镜像 OK" 3)) { $out.Add("      " + $x) }
$nOutDown = Cnt "出向回写不启用"
$nOutDown2 = Cnt "出向回写停用"
$out.Add("  [2d] 关闸行 (不启用/停用) = " + ($nOutDown + $nOutDown2) + "   [>0 => 看下方明细, 且不许连坐入向/PoC-B]")
foreach ($x in (Pick "出向回写" 6)) { $out.Add("      " + $x) }
$nOutReady = Cnt "[2d] 出向图就绪"
$out.Add("  [2d] 出向图就绪 = $nOutReady   [应为1]")
foreach ($x in (Pick "[2d] 出向图就绪" 3)) { $out.Add("      " + $x) }
$nOutSub = Cnt "[2d] VK出向#"
$out.Add("  [2d] VK出向# 行数 = $nOutSub   [前8条+每128条 => 应随帧数增长]")
foreach ($x in (Pick "[2d] VK出向#" 5)) { $out.Add("      " + $x) }
$nOutWr = Cnt "[2d] 回写#"
$nOutDescOk = CntBoth "[2d] 回写#" "[desc一致]"
$nOutDescBad = CntBoth "[2d] 回写#" "[desc不一致!"
$out.Add("  [2d] 回写# 行数 = $nOutWr   [前8条+每128条]")
$out.Add("      desc一致 = $nOutDescOk   desc不一致 = $nOutDescBad   [不一致必须为0, 否则 CopyResource 静默丢弃]")
foreach ($x in (Pick "[2d] 回写#" 6)) { $out.Add("      " + $x) }
$nOutChk = Cnt "[2d] 出向读回#"
$nOutOk  = CntBoth "[2d] 出向读回#" "**一致✓**"
$nOutBad = CntBoth "[2d] 出向读回#" "不一致✗"
$out.Add("  [2d] 出向读回 行数 = $nOutChk   一致 = $nOutOk   不一致 = $nOutBad   [前3+每600; ≥1 一致 = passthrough 字节还原; v0.18.3 起 2c 后 Flush, 预期不一致 = 0]")
foreach ($x in (Pick "[2d] 出向读回#" 6)) { $out.Add("      " + $x) }
$m6 = [regex]::Match($lastSum, "出向=(\d+)")
$nOut = if ($m6.Success) { [int]$m6.Groups[1].Value } else { -1 }
$mFr = [regex]::Match($lastSum, "帧=(\d+)")
$nFr = if ($mFr.Success) { [int]$mFr.Groups[1].Value } else { 0 }
# 12f 上界 (C-6, 2026-10-06 实测修复前 28.7 次/帧): 设计 = 每帧 1 次
$ratio = if ($nFr -gt 0 -and $nOut -ge 0) { [math]::Round($nOut / $nFr, 2) } else { -1 }
$out.Add("  出向= 末值 = $nOut   帧= $nFr   出向/帧 = $ratio   [vkout=1 应 ≈1.0 (±20%); C-6 修复前实测 28.7]")
$out.Add("")

$out.Add("==== #13 2d-3 深度格式探测 (v0.18.3) ====")
$nP13 = Cnt "[2d-3]   格式探测#"
$out.Add("  [2d-3] 格式探测 行数 = $nP13   [深度 SHARED 建不成时应 = 7; 深度建成了则 0, 不算 FAIL]")
foreach ($x in (Pick "[2d-3]   格式探测#" 8)) { $out.Add("      " + $x) }
$nP13nt = CntBoth "[2d-3]   格式探测#" "handle=OK(NT)"
$out.Add("  NT-handle 可用的格子 = $nP13nt   [>0 => 路线1 前置成立 (深度可转进该格式的 SHARED 镜像, VK D3D11_TEXTURE_BIT 能导入)]")
$nP13c = Cnt "[2d-3]   格式探测 结论"
$out.Add("  [2d-3] 结论行 = $nP13c   [有探测行时应为1]")
foreach ($x in (Pick "[2d-3]   格式探测 结论" 2)) { $out.Add("      " + $x) }
$out.Add("")

$out.Add("==== #14 2d-4 路线1' KMT 探测 (v0.18.3) ====")
$nP14 = Cnt "[2d-4]   KMT探测#"
$out.Add("  [2d-4] KMT探测 行数 = $nP14   [应 = 4: #1/#2 D3D11 两档 BindFlags + #3/#4 VK; 0 = 探测没跑]")
foreach ($x in (Pick "[2d-4]   KMT探测#" 6)) { $out.Add("      " + $x) }
$nKmtOld = CntBoth "[2d-4]   KMT探测#" "老式handle=OK"
$out.Add("  D3D11 老式 handle OK 格子数 = $nKmtOld   [>=1 => D24 家族单独 SHARED 可用, 病因钉死在 NTHANDLE 这一轴]")
$nKmtBind = CntBoth "[2d-4]   KMT探测#" "bind=0 (0x00000000)"
$out.Add("  VK 导入+绑定 成功行 = $nKmtBind   [>=1 => 路线1' 前置成立: 深度可原样直入 VK, 省掉路线1 的每帧全屏 PS]")
$nP14c = Cnt "[2d-4]   KMT探测 结论"
$out.Add("  [2d-4] 结论行 = $nP14c   [有探测行时应为1]")
foreach ($x in (Pick "[2d-4]   KMT探测 结论" 2)) { $out.Add("      " + $x) }
$out.Add("")

$out.Add("==== 末 10 行原始日志 ====")
foreach ($x in ($lines | Select-Object -Last 10)) { $out.Add("  " + $x) }

$out.Add("")
$out.Add("==== 自动结论 ====")
$fail = New-Object System.Collections.Generic.List[string]
$warn = New-Object System.Collections.Generic.List[string]
# --- 根因短路 (2026-10-06 踩坑): 挂载门没过 => 下面几乎每条 FAIL 都是它的下游 ---
if ($hookN -ne 1) {
    $root = "#1 挂载门没过 (ctx槽33/50/47 未挂) => 33/47/50 钩子全没装, OM/候选/特征A,B/入向/出向 恒为 0"
    if ((Cnt "挂载门未满足") -gt 0) {
        $root += " ;日志已直接点名 -> 看那行括号里的 dev/vulkan/probe/ssr 四个值, 谁是 0 谁就是关着的开关 (v0.18.1 起 C-5 会打这行)"
    } elseif ((Cnt "探针升质: ini probe=0") -gt 0) {
        $root += " ;本页已确认 ini probe=0 -> installSsrRecon 第一行静默 return (main.cpp:2089 门 = pocbEnabled && g_probeOn && g_ssrOn; g_probeOn = iniFlag(""probe"", true))"
        $root += " ;修法 = ini 写 probe=1 后重跑 (probe 默认就是 1, 别手写 0)"
    } else {
        $root += " ;看日志有无 'GetImmediateContext 空 / slot33/50 原值不在 d3d11 / ctx 登记表已满'"
    }
    $out.Add("  [根因] " + $root)
    $out.Add("         -> 本页其余 FAIL 均为该根因的下游; 换 DLL、重跑图之前先确认 ini 五个开关")
}
if ($ver -ne "v0.18.3") { $fail.Add("#0 banner 不是 v0.18.3 -> DLL 没换") }
if ($n_shr -ne 1) { $fail.Add("#1 ini ssr.shared=1 未读到 -> ini 没写或 ssr=0") }
if ($n_sen -gt 0) { $fail.Add("#1 ssr.sentinel 还开着 -> 应为0, 否则水会消失干扰判读") }
if ($n_c -ne 1) { $fail.Add("#2 色镜像 OK 行 != 1") }
$dOK = ($n_d -eq 1)
if (-not $dOK) { $warn.Add("#3 深度镜像 OK 行 != 1 -> R2 深度不过 (看 #4 归因; v0.16.0 已归因为格式=已知)") }
if ($r2 -gt 0) { $warn.Add("#4 R2 风险点命中 $r2 次 -> R2 半过 (色过/深度不过, 归因=格式则已知)") }
if ($n_ready -ne 1) { $fail.Add("#5 就绪行 != 1") }
if ($inN -le 0) { $fail.Add("#8 入向累计=0 -> 没触发 (菜单期不 arm? 没进实机场景?)") }
if ($n7 -eq 0) { $fail.Add("#7 没有 [2c读回 行 -> 节流逻辑或触发有问题") }
if ($nBad -gt 0) { $fail.Add("#7 读回 =基线 -> CopyResource 静默失败, desc 不匹配") }
if ($nSen -gt 0) { $fail.Add("2b 哨兵还在执行 -> ssr.sentinel 没关") }
if ($nAnom -gt 0) { $fail.Add("#11 有 [异常] -> 看上面明细") }
# --- 2c-β ---
if ($nB1 -ne 1) { $fail.Add("β2 [2c-β] VK 导入 OK != 1 -> 单参数导入没成 (看 [矩阵]/[探针]/[结论]/'建图失败换槽' 行)") }
if ($nB2 -eq 0) { $fail.Add("β3 没有交叉校验行 -> 没进实机场景, 或 2c-β 没跑 (PoC-B 挂了的话它也不跑)") }
if ($nBok -eq 0 -and $nB2 -gt 0) {
    $fail.Add("β3 跑了 $nB2 次交叉校验但 0 次一致 -> D3D11_TEXTURE_BIT 也不对 (看 [v0.16.3 归因])")
}
if ($nBbad -gt 0 -and $nBok -gt 0) {
    $fail.Add("β3 混合: $nBok 次一致 + $nBbad 次不一致 -> 已定案的单参数不该抖动 (看不一致那几行的 槽K/归因字段)")
}
if ($nBok -gt 0 -and $nBok -lt 4) {
    $warn.Add("β3 一致样本只有 $nBok 次 (<4) -> 收口证据偏少, 建议多跑一段再定")
}
if ($nDLost -gt 0) { $fail.Add("2c-β 自己的提交 DEVICE_LOST -> 看 [异常] 行") }
if ($nPocbDL -gt 0) { $fail.Add("PoC-B 报 code=-4 (设备丢) -> 这次读回/建图把 VkDevice 搞丢了, 三角会消失, 看上面 PoC-B 失败行") }
# --- 2d-1 出向回写 (v0.18.1) ---
if ($nVkoIni -eq 0) {
    $warn.Add("2d ini ssr.vkout 没读到 -> ini 没写 (或 ssr/shared 没开), 本轮回退纯 2c 形态")
} else {
    if ($nOutImg -ne 1) { $fail.Add("2d 出向镜像 OK != 1 -> 第3张 SHARED 镜像没建成 (看 [2d] 关闸行)") }
    if ($nOutReady -ne 1) { $fail.Add("2d [2d] 出向图就绪 != 1 -> VK 导入/录命令没成 (看 [2d] 失败行)") }
    if ($nOutWr -eq 0) { $fail.Add("2d 没有 [2d] 回写 行 -> g_ssrOutReady 没置上, 或 585 guard 没过 (双强特征格局?)") }
    if ($ratio -ge 0 -and $nFr -ge 500) {
        if ($ratio -gt 1.5) { $fail.Add("2d 回写 = $ratio 次/帧 (应 ≈1) -> C-6 一次性门没生效 (g_ssrOutPending 没置位/没消费)") }
        elseif ($ratio -lt 0.5) { $warn.Add("2d 回写只有 $ratio 次/帧 -> 一次性门过紧或特征B 没 arm (看 [2d出向已排队] 行)") }
    }
    if ($nOutDescBad -gt 0) { $fail.Add("2d 回写 desc不一致 $nOutDescBad 次 -> CopyResource 会静默丢弃") }
    if ($nOutOk -eq 0) { $fail.Add("2d 出向读回 0 次一致 -> passthrough 没逐字节还原 (看上面不一致行的入向/出向值)") }
    # --- O-1 验收 (v0.18.3): 2c 拷贝后 Flush 应把"差一帧不一致"消干净 => 预期 0 次不一致 ---
    if ($nOutBad -gt 0) {
        $warn.Add("2d 出向读回有 $nOutBad 次不一致 -> O-1 的 2c 后 Flush 没把差一帧消干净 (v0.18.2 实测 5 次; 根因 = 零跨API栅栏下 D3D11 拷贝还没交 GPU, VK 已读)")
    }
    # --- C-7 验收 (v0.18.2): 节流计数须进门先推进 => 行数 ≈ 3 + 事件数/600, 事件数 ≈ 0.68*帧数 ---
    $minOut = [Math]::Max(3, [int][math]::Floor($nFr / 1000))
    if ($nFr -ge 1200 -and $nOutChk -lt $minOut) {
        $warn.Add("2d 出向读回只有 $nOutChk 行 (< $minOut) -> C-7 节流计数没生效 (应进门先 ++ 再判节流)")
    }
    if ($nOutDown + $nOutDown2 -gt 0) { $fail.Add("2d 关闸了 -> 看上面 [2d] 关闸行 (出向任一步失败只关自己)") }
    if ($nSen -gt 0) { $fail.Add("2d 开着但 2b 哨兵还在执行 -> 让位逻辑没生效, 585 归因不纯") }
}
# --- 2d-3 深度格式探测 (v0.18.2) ---
if ($n_d -ne 1 -and $nP13 -eq 0) {
    $warn.Add("2d-3 深度 SHARED 没成, 但没有 [2d-3] 格式探测行 -> 探测没跑 (静态 flag 已置? nm 不含 '深度'?)")
}
if ($nP13 -gt 0 -and $nP13c -eq 0) { $fail.Add("2d-3 有探测行但没有结论行 -> 结论打印被跳过, 路线1/2 无法定案") }
# --- 2d-4 路线1' KMT 探测 (v0.18.3) ---
if ($nP14 -gt 0 -and $nP14c -eq 0) { $fail.Add("2d-4 有探测行但没有结论行 -> 路线1' 无法定案 (看 #3/#4 卡在哪一步)") }
if ($n_d -ne 1 -and $nP14 -eq 0) {
    $warn.Add("2d-4 没有 KMT 探测行 -> 路线1' 没测 (与 2d-3 同一触发点, 深度 SHARED 没成时应一起跑)")
}
if ($nP14 -gt 0) {
    if ($nKmtBind -gt 0) { $out.Add("  [2d-4] 判读: 路线1' 前置成立 -> 深度可原样直入 VK, 省掉每帧全屏 PS (落地前按 docs/05 D2a-4 另评导入分支)") }
    else { $out.Add("  [2d-4] 判读: 路线1' 前置不成立 -> 回到路线1 (R32_FLOAT x BindFlags 0x28 全屏 PS, 前置已由 2d-3 6/6 验通)") }
}
if ($warn.Count -gt 0) {
    $out.Add("  [已知/告警] " + $warn.Count + " 项:")
    foreach ($w in $warn) { $out.Add("    ~ " + $w) }
}
if ($fail.Count -eq 0) {
    $r2txt = $(if ($dOK) { "全过 (D24 深度 + FP16 都可建 SHARED 且 DXGI 允许共享)" } else { "色过/深度不过, 归因=格式 (R2 病因=格式, 深度需改道)" })
    $out.Add("  [PASS] 全绿 -> 2c-alpha + 2c-beta 收口, R2 " + $r2txt)
    $out.Add("  入向通路 (D3D11 写 -> VK 读) 验通: 交叉校验 $nBok 次一致 (v0.16.6 定案 handleType=D3D11_TEXTURE_BIT)")
    if ($nVkoIni -gt 0) {
        $out.Add("  2d-1 出向回写落地: 回写 $nOutWr 行 desc一致 $nOutDescOk, 出向读回一致 $nOutOk 次")
        $out.Add("  下一步: 按 [2d-3]/[2d-4] 结论定深度改道 —— 路线1' (KMT 直入, 省每帧全屏 PS) 或 路线1 (R32_FLOAT 全屏 PS) -> SSR v1 shader 采样")
    } else {
        $out.Add("  下一步: 2d-1 本轮回退 (ini 没开 ssr.vkout); 开了再跑一轮即可验出向")
    }
} else {
    $out.Add("  [FAIL] " + $fail.Count + " 项不符:")
    foreach ($f in $fail) { $out.Add("    - " + $f) }
}

[System.IO.File]::WriteAllLines("$base\run2c.out", $out, (New-Object System.Text.UTF8Encoding($false)))
Write-Output ("wrote " + "$base\run2c.out")

