param([string]$After = '', [string]$Log = '')
# dllchk.ps1  -- gate: is the DLL the game ACTUALLY LOADS newer than the build?
#
# Why this exists: a run was wasted because the ini/knobchk both passed while the
# DLL on disk was still the previous artifact (mtime 05:36, build from 12:xx).
# knobchk proves the knob was read; it says nothing about which binary read it.
#
# Usage:
#   dllchk.ps1                       -> prints path/mtime/size + which file the log says loaded
#   dllchk.ps1 -After "2026-10-08 13:20:00"   -> exit 0 iff that file is NOT older than the time
#                                                (pass the artifact/commit time you expect)
#
# ASCII-only; CRLF no BOM (tools convention).
$ErrorActionPreference = 'Stop'

$dll = 'D:\The Elder Scrolls V Skyrim\Data\SKSE\Plugins\poc-presenter.dll'
if ($Log -eq '') {
    $Log = 'C:\Users\joker\AppData\Local\ModOrganizer\Skyrim Special Edition fsr\overwrite\SKSE\Plugins\poc-presenter.log'
}

if (-not (Test-Path -LiteralPath $dll)) { Write-Output ("MISSING " + $dll); exit 2 }
$f = Get-Item -LiteralPath $dll
Write-Output ("dll    " + $f.FullName)
Write-Output ("mtime  " + $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + "   size " + $f.Length)

# what the log says the process actually mapped (authoritative: hook reports its own module)
$loaded = ''
if (Test-Path -LiteralPath $Log) {
    $lines = [System.IO.File]::ReadAllLines($Log, [System.Text.Encoding]::UTF8)
    for ($i = $lines.Count - 1; $i -ge 0; $i--) {
        $m = [regex]::Match($lines[$i], '([A-Za-z]:\\.*poc-presenter\.dll) ==')
        if ($m.Success) {
            $loaded = $m.Groups[1].Value
            Write-Output ("loaded " + $loaded + "   (log " + $lines[$i].Substring(1, 19) + ")")
            break
        }
    }
}
if ($loaded -eq '') { Write-Output "loaded <not found in log yet>" }
elseif ($loaded -ne $dll) {
    Write-Output ("VERDICT: FAIL - the game loaded a DIFFERENT file than " + $dll)
    exit 1
}

if ($After -eq '') {
    Write-Output 'NOTE: pass -After "yyyy-MM-dd HH:mm:ss" (artifact/commit time) for a verdict.'
    exit 0
}

$t = [datetime]::ParseExact($After, 'yyyy-MM-dd HH:mm:ss', $null)
$age = $t - $f.LastWriteTime
if ($f.LastWriteTime -ge $t) {
    Write-Output ("VERDICT: PASS - DLL mtime " + $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss') + " >= " + $After)
    exit 0
}
Write-Output ("VERDICT: FAIL - DLL is " + [int]$age.TotalMinutes + " min OLDER than " + $After + " => STALE BUILD, do not trust any shot")
exit 1
