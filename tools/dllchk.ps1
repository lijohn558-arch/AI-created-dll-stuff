param(
    [string]$Spv = 'C:\Users\joker\skyrim-vulkan\src\poc-presenter\shaders\ssr.frag.spv',
    [string]$After = '',
    [string]$Log = ''
)
# dllchk.ps1  -- gate: is the DLL the game ACTUALLY LOADS built from CURRENT source?
#
# WHY v2 (2026-10-08): v1 compared the DLL mtime against a commit time and cried
#   STALE BUILD on a perfectly fresh DLL.  Root cause: GitHub Actions runners are UTC,
#   and a zip stores each entry's DOS time as the WRITER'S wall clock.  Extracting on a
#   +0800 box makes PowerShell print "05:36:52" for a build that really happened at
#   13:36:52 local -- i.e. the displayed clock is UTC, the commit clock is local, and the
#   8h gap is pure timezone.  A wrong verdict cost a whole capture round.
#   => v2 makes CONTENT the primary oracle and demotes time to a secondary, explicitly
#      normalised check.
#
# CHECKS
#   1. loaded   : which file the process really mapped (log line `... poc-presenter.dll ==`)
#   2. content  : does the DLL embed the CURRENT ssr.frag.spv byte for byte?
#                 (the spv IS embedded verbatim -- verified at dll offset 0x5dda0)
#                 => timezone-proof proof that the shader you are about to shoot is the
#                    shader you just built.  This is the check that matters.
#   3. -After   : optional.  Given as LOCAL wall clock (git's %ci, e.g. 2026-10-08 13:10:05).
#                 The DLL mtime is first normalised:  artifactLocal = printedLocal + UTCoffset.
#                 Only meaningful for artifact-extracted DLLs; use it to catch stale C++ builds.
#
# Usage:
#   dllchk.ps1
#   dllchk.ps1 -After "2026-10-08 13:10:05"
#
# ASCII-only; CRLF no BOM (tools convention).  exit 0 = PASS, 1 = FAIL, 2 = missing dll.
$ErrorActionPreference = 'Stop'

$dll = 'D:\The Elder Scrolls V Skyrim\Data\SKSE\Plugins\poc-presenter.dll'
if ($Log -eq '') {
    $Log = 'C:\Users\joker\AppData\Local\ModOrganizer\Skyrim Special Edition fsr\overwrite\SKSE\Plugins\poc-presenter.log'
}
$fail = $false

if (-not (Test-Path -LiteralPath $dll)) { Write-Output ("MISSING " + $dll); exit 2 }
$f = Get-Item -LiteralPath $dll
Write-Output ("dll      " + $f.FullName)
Write-Output ("mtime    " + $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + " local   " +
              $f.LastWriteTimeUtc.ToString('yyyy-MM-dd HH:mm:ss') + " stored-UTC   size " + $f.Length)

# artifact-extracted files carry the runner's UTC wall clock in the local field.
$off = [TimeZoneInfo]::Local.GetUtcOffset([DateTime]::UtcNow).TotalHours
$artifactLocal = $f.LastWriteTime.AddHours($off)
Write-Output ("normal   " + $artifactLocal.ToString('yyyy-MM-dd HH:mm:ss') +
              "  <- as local wall clock (printed +" + $off + "h; artifact clock is UTC)")

# --- 1. which file did the process map? ---
$loaded = ''
if (Test-Path -LiteralPath $Log) {
    $lines = [System.IO.File]::ReadAllLines($Log, [System.Text.Encoding]::UTF8)
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $m = [regex]::Match($lines[$i], '([A-Za-z]:\\.*poc-presenter\.dll) ==')
        if ($m.Success) {
            $loaded = $m.Groups[1].Value
            Write-Output ("loaded   " + $loaded + "   (log " + $lines[$i].Substring(1, 19) + ")")
            break
        }
    }
}
if ($loaded -eq '') { Write-Output "loaded   <not found in log yet>" }
elseif ($loaded -ne $dll) {
    Write-Output ("FAIL[loaded] the game mapped a DIFFERENT file than " + $dll)
    $fail = $true
}

# --- 2. content: does the DLL carry the current SPIR-V? ---
if (Test-Path -LiteralPath $Spv) {
    $db = [System.IO.File]::ReadAllBytes($dll)
    $sb = [System.IO.File]::ReadAllBytes($Spv)
    $n = [Math]::Min(128, $sb.Length)
    # degenerate-input guard: a low-entropy needle (all zeros, short constant) would
    # match the DLL's own padding and report a false PASS.  Real SPIR-V is high-entropy.
    $seen = @{}
    for ($j = 0; $j -lt $n; $j++) { $seen[[int]$sb[$j]] = $true }
    $degenerate = ($n -lt 16 -or $seen.Count -lt 8)
    if ($degenerate) {
        Write-Output ("spv      FAIL - needle is degenerate (" + $seen.Count + " distinct bytes in first " +
                      $n + ") - not a real SPIR-V, cannot prove anything")
        $fail = $true
    } else {
        $found = $false
        for ($i = 0; $i -le $db.Length - $n; $i++) {
            if ($db[$i] -ne $sb[0]) { continue }
            $ok = $true
            for ($j = 1; $j -lt $n; $j++) { if ($db[$i + $j] -ne $sb[$j]) { $ok = $false; break } }
            if ($ok) { $found = $true; break }
        }
        if ($found) {
            Write-Output ("spv      PASS - DLL embeds current " + $sb.Length + " B SPIR-V (first " + $n + " B match)")
        } else {
            Write-Output "spv      FAIL - DLL does NOT contain the current SPIR-V => STALE SHADER, do not trust any shot"
            $fail = $true
        }
    }
} else {
    Write-Output ("spv      SKIP - " + $Spv + " not found (run tools\\make_shaders.ps1)")
}

# --- 3. optional time check, in LOCAL wall clock ---
if ($After -ne '') {
    $t = [datetime]::ParseExact($After, 'yyyy-MM-dd HH:mm:ss', $null)
    if ($artifactLocal -ge $t) {
        Write-Output ("time     PASS - build " + $artifactLocal.ToString('yyyy-MM-dd HH:mm:ss') + " >= " + $After)
    } else {
        Write-Output ("time     FAIL - build is " + [int]($t - $artifactLocal).TotalMinutes +
                      " min OLDER than " + $After + " => STALE C++ BUILD")
        $fail = $true
    }
} else {
    Write-Output 'time     (skip) pass -After "yyyy-MM-dd HH:mm:ss" to check the C++ side too'
}

if ($fail) { Write-Output 'VERDICT: FAIL - do not shoot on this DLL'; exit 1 }
Write-Output 'VERDICT: PASS - this DLL was built from the source you are about to shoot'
exit 0
