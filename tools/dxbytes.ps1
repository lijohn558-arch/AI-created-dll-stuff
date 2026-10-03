# dxbytes.ps1 — 读本机 dxgi.dll 三个存根函数的前导字节 (v1.7 方案B 字节守卫的对照源)
#
# 用途: Windows 更新导致 dxgi 版本漂移、poc-presenter 日志出现
#   "方案B ... 入口字节与预期不符 ... 保守跳过" 时, 运行本脚本取当前真实字节,
#   据此更新 src/poc-presenter/main.cpp 里的 kPresentBytes/kPresent1Bytes 常量与 RVA。
# 用法: powershell -NoProfile -ExecutionPolicy Bypass -File tools\dxbytes.ps1
# 只读: 仅 LoadLibrary(dxgi) + 按 RVA 读字节, 不创建任何 D3D 对象、不改任何内存。
#
# 槽位勘误 (v1.7, MS dxgi.h + 本地实证):
#   Present=槽8  → dxgi+0x19000 (旧 0x2E460 实为槽4 SetPrivateDataInterface 存根)
#   Present1=槽22 → dxgi+0x194A0 (旧 0x4EE60 实为槽18 GetDesc1 存根)
#   旧 0x2E460/0x4EE60 条目保留仅供解读 v1.0~v1.6 老日志。

$src = @'
using System;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

public static class DxBytes
{
    [DllImport("kernel32", CharSet = CharSet.Ansi)]
    public static extern IntPtr LoadLibraryA(string name);

    public static string Module(long addr)
    {
        foreach (ProcessModule m in Process.GetCurrentProcess().Modules)
        {
            long b = m.BaseAddress.ToInt64();
            if (addr >= b && addr < b + m.ModuleMemorySize)
                return m.ModuleName + "+0x" + (addr - b).ToString("X") +
                       " (base 0x" + b.ToString("X") + ")";
        }
        return "?";
    }

    public static string Dump(long addr, int n)
    {
        if (Module(addr) == "?")
            return "(地址不在任何已加载模块内, 跳过读取)";
        var sb = new StringBuilder();
        for (int i = 0; i < n; i++)
            sb.Append(Marshal.ReadByte((IntPtr)addr, i).ToString("X2")).Append(' ');
        return sb.ToString();
    }
}
'@

Add-Type -TypeDefinition $src
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8  # 中文判定行在控制台正常显示

$base = [DxBytes]::LoadLibraryA("dxgi.dll")
if ($base -eq [IntPtr]::Zero) {
    Write-Error "dxgi.dll 加载失败"
    exit 1
}
$baseAddr = $base.ToInt64()

$targets = @(
    @{ Name = "Present(slot8)";    Rva = 0x19000; Expect = "48 89 5C 24 10 48 89 74 24 18 55 57 41 56";
       Note = "v1.7 方案B 正主 (kPresentBytes, 14B, 边界14)" },
    @{ Name = "Present1(slot22)";  Rva = 0x194A0; Expect = "48 89 5C 24 10 48 89 74 24 18 55 57 41 54";
       Note = "v1.7 方案B 正主 (kPresent1Bytes, 14B, 边界14/16)" },
    @{ Name = "CreateSwapChain(10)"; Rva = 0x2C4D0; Expect = $null;
       Note = "旧记录留档 — 身份未证, 权威值看日志 '探针: CreateSwapChain 原实现前32字节'" },
    @{ Name = "旧标Present(实slot4)"; Rva = 0x2E460; Expect = "48 83 EC 38 4C 89 44 24 50 4C 8D 4C 24 50";
       Note = "历史遗留 — 实为 SetPrivateDataInterface 存根 (v1.0~v1.6 自证循环)" },
    @{ Name = "旧标Present1(实slot18)"; Rva = 0x4EE60; Expect = "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20";
       Note = "历史遗留 — 实为 GetDesc1 存根 (v1.0~v1.6 自证循环)" }
)

Write-Host ("dxgi {0}" -f [DxBytes]::Module($baseAddr))
foreach ($t in $targets) {
    $addr = $baseAddr + $t.Rva
    $b32 = [DxBytes]::Dump($addr, 32)
    if ($t.Expect) {
        if ($b32.Replace(" ", "").StartsWith($t.Expect.Replace(" ", ""))) {
            $status = if ($t.Name.StartsWith("旧")) { "与 v1.6 记录一致 (历史条目)" } else { "与 v1.7 期望一致 OK" }
        } else {
            $status = if ($t.Name.StartsWith("旧")) { "!! 与 v1.6 记录不符 (dxgi 已更新, 老日志不可再比对)" } else { "!! 漂移 — 需按此字节更新 main.cpp 的 kPresentBytes/kPresent1Bytes 与 RVA" }
        }
    } else {
        $status = "仅留档 (方案C: 不挂工厂, 无需守卫)"
    }
    Write-Host ("{0,-18} @ dxgi+0x{1:X} = 0x{2:X}" -f $t.Name, $t.Rva, $addr)
    Write-Host ("    前32字节: {0}" -f $b32)
    Write-Host ("    判定: {0}" -f $status)
    if ($t.Note) { Write-Host ("    备注: {0}" -f $t.Note) }
}
