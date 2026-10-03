# make_shaders.ps1 — GLSL -> SPIR-V -> C 头文件 (PoC-B 注入着色器)
#
# 为什么有这一步: 本机无原生编译器、CI 也无 Vulkan SDK, 故用 RenderDoc 自带的
# glslangValidator 在本地把 GLSL 编成 SPIR-V, 再生成字节数组头文件随源码入库 ——
# CI 只需编 main.cpp (含该头), 不依赖任何 shader 工具链。
#
# 用法:
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_shaders.ps1
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_shaders.ps1 -Glslang "D:\...\glslangValidator.exe"
#
# 产出:
#   src\poc-presenter\shaders\pocb.vert.spv / pocb.frag.spv  (中间产物, 便于人查)
#   src\poc-presenter\pocb_shaders.h                          (main.cpp include 的字节数组)
#
# 判定: 两个 .spv 生成 + 头文件含 kPocbVertSpv / kPocbFragSpv 即成功。
param(
    [string]$Glslang = "C:\Program Files\RenderDoc\plugins\spirv\glslangValidator.exe",
    [string]$OutHeader = ""
)

$ErrorActionPreference = "Stop"
$tools = $PSScriptRoot
$root = Split-Path $tools
$shDir = Join-Path $root "src\poc-presenter\shaders"
if (-not $OutHeader) { $OutHeader = Join-Path $root "src\poc-presenter\pocb_shaders.h" }
if (-not (Test-Path $Glslang)) { throw "glslangValidator 不存在: $Glslang" }

function Compile-Spv([string]$src, [string]$stage, [string]$out) {
    & $Glslang -V -S $stage -o $out $src
    if ($LASTEXITCODE -ne 0) { throw "编译失败 (exit=$LASTEXITCODE): $src" }
    if (-not (Test-Path $out)) { throw "未产出 SPV: $out" }
}

function Emit-Array([string]$name, [string]$spvPath) {
    $bytes = [System.IO.File]::ReadAllBytes($spvPath)
    if ($bytes.Length -eq 0 -or ($bytes.Length % 4) -ne 0) {
        throw "SPIR-V 长度异常 (须非 0 且 4 字节倍数): $spvPath ($($bytes.Length) B)"
    }
    $sb = New-Object System.Text.StringBuilder
    [void]$sb.AppendLine("static const unsigned int $name[] = {")
    for ($i = 0; $i -lt $bytes.Length; $i += 4) {
        $w = [System.BitConverter]::ToUInt32($bytes, $i)
        $comma = if ($i + 4 -lt $bytes.Length) { "," } else { "" }
        $nl = if (($i / 4) % 8 -eq 7 -or ($i + 4) -ge $bytes.Length) { "`r`n" } else { " " }
        if ($i % 32 -eq 0) { [void]$sb.Append("    ") }
        [void]$sb.Append(("0x{0:x8}u{1}{2}" -f $w, $comma, $nl))
    }
    [void]$sb.AppendLine("};")
    return $sb.ToString()
}

$vertSrc = Join-Path $shDir "pocb.vert"
$fragSrc = Join-Path $shDir "pocb.frag"
foreach ($f in @($vertSrc, $fragSrc)) { if (-not (Test-Path $f)) { throw "缺 GLSL 源: $f" } }

$vertSpv = Join-Path $shDir "pocb.vert.spv"
$fragSpv = Join-Path $shDir "pocb.frag.spv"
Compile-Spv $vertSrc "vert" $vertSpv
Compile-Spv $fragSrc "frag" $fragSpv

$sb = New-Object System.Text.StringBuilder
[void]$sb.AppendLine("// pocb_shaders.h — 由 tools\make_shaders.ps1 生成, 请勿手改")
[void]$sb.AppendLine("//")
[void]$sb.AppendLine("// 源: src\poc-presenter\shaders\pocb.vert + pocb.frag (GLSL 450 -> SPIR-V 1.x)")
[void]$sb.AppendLine("// 再生成: powershell -NoProfile -ExecutionPolicy Bypass -File tools\make_shaders.ps1")
[void]$sb.AppendLine("//")
[void]$sb.AppendLine("// SPIR-V 规范要求 module 大小是 4 的倍数, 且 pCode 按 uint32 对齐 → 这里存字。")
[void]$sb.AppendLine("#pragma once")
[void]$sb.AppendLine("")
[void]$sb.AppendLine((Emit-Array "kPocbVertSpv" $vertSpv))
[void]$sb.AppendLine((Emit-Array "kPocbFragSpv" $fragSpv))
[void]$sb.AppendLine(("static const unsigned int kPocbVertSpvBytes = sizeof(kPocbVertSpv);"))
[void]$sb.AppendLine(("static const unsigned int kPocbFragSpvBytes = sizeof(kPocbFragSpv);"))

# UTF-8 无 BOM (内容为 ASCII + 注释), 与 MSVC /utf-8 兼容
[System.IO.File]::WriteAllText($OutHeader, $sb.ToString(), (New-Object System.Text.UTF8Encoding($false)))

"OK: $OutHeader"
"    vert.spv = $((Get-Item $vertSpv).Length) B, frag.spv = $((Get-Item $fragSpv).Length) B"
"    头文件行数 = $((Get-Content $OutHeader).Count)"
