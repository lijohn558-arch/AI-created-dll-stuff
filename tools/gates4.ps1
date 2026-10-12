# gates4.ps1 -- run the five pre-push gates in one shot. Pure ASCII (PS 5.1 reads
# unmarked files as ANSI, so any CJK literal would mojibake).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\gates4.ps1
# Gates: check1 / quotescan3 / verify_banner / enc_gate / spvsync
# Prints every gate's output and a final "GATES4: ALL PASS" / "GATES4: FAILED".
param([string]$Repo = "C:\Users\joker\skyrim-vulkan")
$ErrorActionPreference = "Continue"
$tools = Join-Path $Repo "tools"

$gates = @("verify_banner", "quotescan3", "enc_gate", "check1", "spvsync")
$failed = @()

foreach ($g in $gates) {
    $p = Join-Path $tools ($g + ".ps1")
    "===== " + $g + " ====="
    if (-not (Test-Path $p)) {
        "MISSING GATE SCRIPT: " + $p
        $failed += $g
        continue
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File $p
    if ($LASTEXITCODE -ne 0) { $failed += $g }
}

if ($failed.Count -eq 0) {
    "GATES4: ALL PASS (check1 / quotescan3 / verify_banner / enc_gate / spvsync)"
    exit 0
}
"GATES4: FAILED -> " + ($failed -join ", ")
exit 1