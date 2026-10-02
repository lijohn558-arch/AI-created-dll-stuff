# rdc_run.ps1 — 无头批跑 RenderDoc 提取脚本 (qrenderdoc --py, 不需要开 UI 手点)
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass5.py -Scene S5
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_run.ps1 -Script rdc_pass6.py -Scene S5 `
#               -Targets "501,520,324,466,352,333" -PsEvents "43712,43825"
#
# 参数:
#   Script    脚本文件名或绝对路径 (默认在本 tools 目录下找)
#   Scene     场景 ID: S1 / S2 / S3 / S4 / S5 (拼 captures\<Scene>.rdc 与 docs\analysis\<Scene>-extract-*.json)
#   Targets   传给 pass6: 要查消费者的资源 ID (逗号分隔, 抓帧局部 ID)
#   PsEvents  传给 pass6: 要导出 PS 反汇编的事件号 (逗号分隔)
#   PsWL      传给 pass6: PS 资源 ID 白名单 (逗号分隔) —— 自动找它首个绑定的 Draw 事件再导出反射+反汇编
#   Stride    传给 pass6: 逐 Draw 步长 (默认 1)
#   QRenderDoc qrenderdoc.exe 路径
#
# 判定: 跑完自动读输出 JSON 的 errors 数组 —— 空 = 成功 (与项目脚本判定口径一致)
param(
    [Parameter(Mandatory = $true)][string]$Script,
    [Parameter(Mandatory = $true)][string]$Scene,
    [string]$Targets = "",
    [string]$PsEvents = "",
    [string]$PsWL = "",
    [string]$Stride = "1",
    [int]$TimeoutSec = 900,
    [string]$QRenderDoc = "C:\Program Files\RenderDoc\qrenderdoc.exe"
)

$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
if (-not [System.IO.Path]::IsPathRooted($Script)) { $Script = Join-Path $tools $Script }
if (-not (Test-Path $Script)) { throw "脚本不存在: $Script" }

$env:RDC_SCENE = $Scene
$env:RDC_TARGETS = $Targets
$env:RDC_PSEVENTS = $PsEvents
$env:RDC_PSWL = $PsWL
$env:RDC_STRIDE = $Stride
$env:RDC_HEADLESS = "1"   # 脚本据此决定结尾 sys.exit(0) (UI 手跑时不设此变量, 不会退出进程)

$analysis = Join-Path (Split-Path $tools) "docs\analysis"
$start = Get-Date

"[{0}] RUN {1} scene={2} targets='{3}' psEvents='{4}' psWL='{5}'" -f (Get-Date -Format "HH:mm:ss"), (Split-Path $Script -Leaf), $Scene, $Targets, $PsEvents, $PsWL
$p = Start-Process -FilePath $QRenderDoc -ArgumentList @("--py", $Script) -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 2 }
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    "TIMEOUT: 超过 $TimeoutSec s 未退出, 已强杀 (多半是脚本没走到结尾, 看下面 LOG)"
} else {
    "exitcode=" + $p.ExitCode
}

# 汇总本轮产出 (按修改时间): json + log + txt —— 输出文件名用短 ID 前缀 (S5-night-combat -> S5-*)
$prefix = ($Scene -split '-')[0]
$files = Get-ChildItem -Path $analysis -File |
    Where-Object { $_.Name -like "$prefix-*" -and $_.LastWriteTime -ge $start.AddSeconds(-2) } |
    Sort-Object Name
if (-not $files) { throw "未找到本轮产出文件 (docs/analysis/$prefix-*)" }

$ok = $true
foreach ($f in $files) {
    "---- {0}  ({1} B, {2})" -f $f.Name, $f.Length, $f.LastWriteTime.ToString("HH:mm:ss")
    if ($f.Extension -eq ".json") {
        $j = Get-Content $f.FullName -Raw | ConvertFrom-Json
        $n = 0
        if ($j.PSObject.Properties.Name -contains "errors") { $n = @($j.errors).Count }
        $el = ""
        if ($j.PSObject.Properties.Name -contains "elapsed_sec") { $el = " elapsed=" + $j.elapsed_sec + "s" }
        "    errors={0}{1}" -f $n, $el
        if ($n -gt 0) {
            $ok = $false
            foreach ($e in @($j.errors)) { "    ! " + $e }
        }
    }
}
$logs = $files | Where-Object { $_.Extension -eq ".log" }
foreach ($l in $logs) {
    "---- LOG tail: {0}" -f $l.Name
    Get-Content $l.FullName -Tail 15 | ForEach-Object { "    " + $_ }
}
if ($ok) { "RESULT: SUCCESS (errors 0)" } else { "RESULT: FAILED" }
