# rdc_compare.ps1 — 配对判定 harness 的无头 runner（对标 tools\rdc_run.ps1）
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4 -Cand S4
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\rdc_compare.ps1 -Base S4 -Cand S2 -Strict
#
# 参数:
#   Base / Cand  基线侧 / 候选侧：场景 ID (S4)、前缀 (S4-water)、或 pass1 JSON 绝对路径
#   Out          报告输出前缀（默认 docs\analysis\<base>~<cand>-compare）
#   Strict       有 DIFF 时以 exit code 1 退出（配对闸门用；默认只报告不失败）
#   TimeoutSec   默认 300s
#   QRenderDoc   qrenderdoc.exe 路径（本机无 Python，qrenderdoc --py 是唯一运行环境）
#
# 判定:
#   1) 报告 JSON errors[] 非空          → 脚本执行失败，抛错
#   2) summary.match = true             → RESULT: MATCH   （无 DIFF 且至少 1 项通过）
#   3) summary.match = false            → RESULT: MISMATCH（-Strict 时 exit 1）
param(
    [Parameter(Mandatory = $true)][string]$Base,
    [Parameter(Mandatory = $true)][string]$Cand,
    [string]$Out = "",
    [switch]$Strict,
    [int]$TimeoutSec = 300,
    [string]$QRenderDoc = "C:\Program Files\RenderDoc\qrenderdoc.exe"
)

$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
$script = Join-Path $tools "rdc_compare.py"
if (-not (Test-Path $script)) { throw "脚本不存在: $script" }

$env:RDC_CMP_BASE = $Base
$env:RDC_CMP_CAND = $Cand
$env:RDC_CMP_OUT = $Out
$env:RDC_HEADLESS = "1"   # rdc_compare.py 据此结尾 sys.exit(0)（UI 手跑不设，不退出进程）

$analysis = Join-Path (Split-Path $tools) "docs\analysis"
if ($Out) { $reportPath = "$Out.json" } else {
    $reportPath = Join-Path $analysis ("{0}~{1}-compare.json" -f ($Base -split '-')[0], ($Cand -split '-')[0])
}
$logPath = [System.IO.Path]::ChangeExtension($reportPath, ".log")

"[{0}] COMPARE base={1} cand={2} strict={3}" -f (Get-Date -Format "HH:mm:ss"), $Base, $Cand, [bool]$Strict
$p = Start-Process -FilePath $QRenderDoc -ArgumentList @("--py", $script) -PassThru
$deadline = (Get-Date).AddSeconds($TimeoutSec)
while (-not $p.HasExited -and (Get-Date) -lt $deadline) { Start-Sleep -Seconds 1 }
if (-not $p.HasExited) {
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    throw "TIMEOUT: 超过 $TimeoutSec s 未退出（看 $logPath）"
}
"exitcode=" + $p.ExitCode

if (Test-Path $logPath) {
    "---- LOG"
    Get-Content $logPath -Encoding UTF8 | ForEach-Object { "    " + $_ }
} else {
    throw "未找到 LOG: $logPath"
}
if (-not (Test-Path $reportPath)) { throw "未找到报告: $reportPath" }

# 报告是 UTF-8（无 BOM）→ 必须显式 -Encoding UTF8，否则 PS5.1 按 ANSI 解码会破坏 JSON 结构
$r = Get-Content $reportPath -Raw -Encoding UTF8 | ConvertFrom-Json
$n = @($r.errors).Count
if ($n -gt 0) {
    foreach ($e in @($r.errors)) { "! " + $e }
    throw "COMPARE FAILED: 脚本执行失败 errors=$n"
}
"summary: pass={0} diff={1} skip={2} total={3} elapsed={4}s" -f `
    $r.summary.pass, $r.summary.diff, $r.summary.skip, $r.summary.total, $r.elapsed_sec
if ($r.summary.pass -eq 0 -and $r.summary.diff -eq 0) {
    "RESULT: NO DATA (锚点全 SKIP — 两侧缺提取产出，先跑 rdc_extract/pass2~6)"
    if ($Strict) { exit 1 }
    exit 0
}
if ($r.match) {
    "RESULT: MATCH (anchors $($r.summary.pass) 通过 / $($r.summary.skip) 跳过)"
    exit 0
} else {
    "RESULT: MISMATCH ($($r.summary.diff) 个锚点 DIFF) — 报告: $reportPath"
    if ($Strict) { exit 1 }
    exit 0
}
