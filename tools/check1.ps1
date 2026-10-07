$ErrorActionPreference = "Stop"
# check1.ps1 —— 提交前结构自检 (v0.17.0 起扫 main.cpp + vkrenderer.cpp + vkrenderer.h)
$dir   = "C:\Users\joker\skyrim-vulkan\src\poc-presenter"
$files = @("main.cpp", "vkrenderer.cpp", "vkrenderer.h")
$strict = New-Object System.Text.UTF8Encoding($false, $true)

$src = @{}
$all = New-Object System.Text.StringBuilder
foreach ($f in $files) {
    $p = Join-Path $dir $f
    $bytes = [System.IO.File]::ReadAllBytes($p)
    try { $s = $strict.GetString($bytes) } catch { Write-Output ("UTF8 decode FAILED " + $f + ": " + $_.Exception.Message); exit 1 }
    if ($bytes.Length -ge 3 -and $bytes[0] -eq 239 -and $bytes[1] -eq 187 -and $bytes[2] -eq 191) { $s = $s.Substring(1) }
    $src[$f] = $s
    [void]$all.Append($s + "`n")
    Write-Output ("UTF8 strict OK  " + $f + "  bytes=" + $bytes.Length)

    # ---- 注释完整性: 误把 "**/" 当 "*/" -> 提前关闭 -> C2146/C4430/C3872 ----
    $openN = [regex]::Matches($s, '/\*').Count
    $closeN = [regex]::Matches($s, '\*/').Count
    $accN  = [regex]::Matches($s, '\*\*/').Count
    $ok = ($openN -eq $closeN -and $accN -eq 0)
    Write-Output ("  comment /*=" + $openN + "  */=" + $closeN + "  accidental '**/'=" + $accN + "  " + $(if ($ok) { "OK" } else { "** FAIL: 注释被提前关 **" }))
    if (-not $ok) { $script:fail = $true }

    # ---- 提取脚本留下的占位标记必须已全部替换掉 (残留 = 编译错) ----
    $mkN = [regex]::Matches($s, '(?m)^\[(SLICE:\d+-\d+|[A-Z]+_PART\d+)\]$').Count
    if ($mkN -ne 0) { Write-Output ("  ** 残留占位标记 = " + $mkN + " **"); $script:fail = $true }

    $c = $s
    $c = [regex]::Replace($c, '/\*[\s\S]*?\*/', ' ')
    $c = [regex]::Replace($c, '(?m)//[^\r\n]*', ' ')
    $c = [regex]::Replace($c, '"(?:\\.|[^"\\])*"', 'S')
    $c = [regex]::Replace($c, "'(?:\\.|[^'\\])*'", 'C')
    foreach ($pair in @(@('{','}'), @('(',')'), @('[',']'))) {
        $o = [regex]::Matches($c, [regex]::Escape($pair[0])).Count
        $z = [regex]::Matches($c, [regex]::Escape($pair[1])).Count
        $d = ($o - $z)
        $flag = ""
        if ($d -ne 0) { $flag = "   <-- ** UNBALANCED **"; $script:fail = $true }
        Write-Output ("  " + $pair[0] + "=" + $o + "  " + $pair[1] + "=" + $z + "  diff=" + $d + $flag)
    }
}
$c = $all.ToString()

# ---- 关键符号 (跨三个文件) ----
$need = @("ssrInFnv", "ssrInMakeShared", "ssrInBuild", "ssrInQueue", "uhex64",
          "g_ssrSharedOn", "g_ssrInPending", "g_ssrInBuilt", "g_ssrDepthRes",
          "g_ssrInBaseC", "g_ssrInBaseD", "g_ssrInStgC", "g_ssrInHC",
          "ssr.shared", "v0.16.0", "g_ssrInN", "g_ssrInLogN",
          "ssrFnvSample", "ssrInVkBuild", "ssrInVkFrame",
          "g_ssrVkState", "g_ssrVkCmd", "g_ssrInChkCValid", "g_ssrInChkC",
          "g_ssrBaseTex", "g_ssrBaseOn", "ssrBaseVkBuild", "ssrMakeSharedTo",
          "g_ssrV1Smooth", "g_ssrV1Blur", "g_ssrV1Debug", "ssrV1DropViewB",
          "g_ssrWDepOn", "g_ssrWDepTex", "g_ssrWDepQ", "g_ssrWDepArm",
          "ssrWDepQueue", "ssrWDepVkBuild", "ssrV1DropViewW", "g_ssrV1ViewW", "ssr.wdep")
foreach ($k in $need) {
    $n = [regex]::Matches($c, [regex]::Escape($k)).Count
    $flag = ""
    if ($n -eq 0) { $flag = "   <-- ** MISSING **"; $script:fail = $true }
    Write-Output ("sym " + $k + " = " + $n + $flag)
}

# ---- 未声明识别符: 用到但没定义 ----
$decl = @{}
foreach ($m in [regex]::Matches($src["main.cpp"], '(?m)^\s*(?:static\s+)?(?:const\s+)?[\w:]+\s*\*?\s*(g_\w+)\s*=')) { $decl[$m.Groups[1].Value] = 1 }
foreach ($m in [regex]::Matches($src["main.cpp"], '(?m)^\s*static\s+[\w:]+\*?\s+(g_\w+)\s*[;\{]')) { $decl[$m.Groups[1].Value] = 1 }
foreach ($m in [regex]::Matches($src["vkrenderer.cpp"], '(?m)^\s*(?:static\s+)?[\w:]+\*?\s+(g_\w+)\s*=')) { $decl[$m.Groups[1].Value] = 1 }
Write-Output ("declared g_ symbols = " + $decl.Count)
foreach ($k in @("g_ssrInBuilt", "g_ssrInDepthFail", "g_ssrInHC", "g_ssrInHD")) {
    $n = $(if ($decl.ContainsKey($k)) { "Y" } else { "N" })
    if ($n -eq "N") { $script:fail = $true }
    Write-Output ("decl " + $k + " = " + $n)
}

# ---- 拆分完整性 (v0.17.0 renderer 模块) ----
$m = $src["main.cpp"]; $v = $src["vkrenderer.cpp"]; $h = $src["vkrenderer.h"]
foreach ($bad in @("g_ssrVkImg", "static PocbCtx g_pocb", "struct SsrVkSlot")) {
    $n = [regex]::Matches($m, [regex]::Escape($bad)).Count
    $flag = ""
    if ($n -ne 0) { $flag = "   <-- ** 应已搬走 **"; $script:fail = $true }
    Write-Output ("main leftover " + $bad + " = " + $n + $flag)
}
foreach ($t in @(@("main.cpp", "namespace pocmain {"), @("main.cpp", "using namespace pocmain;"),
                 @("main.cpp", '#include "vkrenderer.h"'), @("main.cpp", "void pocbFrame(IDXGISwapChain* sc)"),
                 @("vkrenderer.h", "struct PocbCtx"), @("vkrenderer.h", "extern PocbCtx g_pocb;"),
                 @("vkrenderer.h", "namespace pocmain"),
                 @("vkrenderer.cpp", "PocbCtx g_pocb;"), @("vkrenderer.cpp", "using namespace pocmain;"),
                 @("vkrenderer.cpp", "bool pocbInit(IDXGISwapChain* sc)"),
                 @("vkrenderer.cpp", "void pocbInject(PocbCtx& c, IDXGISwapChain* sc)"),
                 @("vkrenderer.cpp", "bool ssrInVkBuild(PocbCtx& c, int slot)"),
                 @("vkrenderer.cpp", "void ssrInVkFrame(PocbCtx& c)"))) {
    $n = [regex]::Matches($src[$t[0]], [regex]::Escape($t[1])).Count
    $flag = ""
    if ($n -eq 0) { $flag = "   <-- ** MISSING **"; $script:fail = $true }
    Write-Output ("has [" + $t[0] + "] " + $t[1] + " = " + $n + $flag)
}
# 头文件里声明的 pocmain 符号, main.cpp 必须有定义 (否则 LNK2019)
foreach ($k in @("void logLine(const std::string& msg)", "std::string pluginDir()", "std::string hexHr(long hr)",
                 "std::string lowerCopy(std::string s)", "bool iniFlag(const char* key, bool def)",
                 "std::string ssrFmtName(DXGI_FORMAT f)", "std::string uhex64(unsigned long long v)",
                 "ssrFnvSample(const void* data", "ssrInFnv(ID3D11DeviceContext* ctx",
                 "void installProbeOn(ID3D11Device* dev)")) {
    $n = [regex]::Matches($m, [regex]::Escape($k)).Count
    $flag = ""
    if ($n -eq 0) { $flag = "   <-- ** MISSING in main **"; $script:fail = $true }
    Write-Output ("pocmain def in main " + $k + " = " + $n + $flag)
}

if ($script:fail) { Write-Output "RESULT FAIL"; exit 1 }
Write-Output "RESULT OK"
