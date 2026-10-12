# ASCII-only. Enable the sky master gate in the game ini (sky=1).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\ini_sky_on.ps1
# Idempotent; value-only replacement preserves trailing comments; LF-only, trailing
# newline kept. Key/value semantics match iniFlag() in main.cpp (key compared
# lowercased, "key = value" spacing allowed).
$ErrorActionPreference = 'Stop'

$p = 'D:\The Elder Scrolls V Skyrim\Data\SKSE\Plugins\poc-presenter.ini'
if (-not (Test-Path -LiteralPath $p)) {
    Write-Output ('MISSING: ' + $p)
    exit 1
}

$bytes = [System.IO.File]::ReadAllBytes($p)
$strict = New-Object System.Text.UTF8Encoding($false, $true)
$ok = $true
try { $null = $strict.GetString($bytes) } catch { $ok = $false }
if (-not $ok) { Write-Output 'ABORT: ini is not valid UTF-8'; exit 1 }

$t = [System.IO.File]::ReadAllText($p)
$before = $t
$t = $t.Replace("`r`n", "`n")

$pat = '(?m)^sky=\d+'
if ([regex]::IsMatch($t, $pat)) {
    $t = [regex]::Replace($t, $pat, 'sky=1')
} else {
    if (-not $t.EndsWith("`n")) { $t = $t + "`n" }
    $t = $t + 'sky=1' + "`n"
}
if (-not $t.EndsWith("`n")) { $t = $t + "`n" }
[System.IO.File]::WriteAllText($p, $t, (New-Object System.Text.UTF8Encoding($false)))
Write-Output 'OK sky=1 written (restart the game; the DLL reads the ini once at startup)'

$chk = [System.IO.File]::ReadAllText($p)
$m = [regex]::Match($chk, '(?m)^sky=\d+.*$')
if ($m.Success) {
    Write-Output ('  ' + $m.Value.Trim())
} else {
    Write-Output '  FAIL: sky line not found after write'
    exit 1
}
if ($before -eq $t) { Write-Output '  (no change: already sky=1)' }