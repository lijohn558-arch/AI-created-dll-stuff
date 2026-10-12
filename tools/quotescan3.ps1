# quotescan3.ps1 -- ASCII gate: added lines in C++/shader sources must carry an
# even number of ASCII double quotes (a stray quote breaks the banner/format
# strings that check1 and the CI build depend on).
# Scans the working-tree diff; pure ASCII on purpose.
param([string]$Repo = "C:\Users\joker\skyrim-vulkan")
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8
Set-Location $Repo
$files = git -c core.quotepath=false diff --name-only
$added = 0
$odd = 0
foreach ($f in $files) {
    if ($f -notlike "*.cpp" -and $f -notlike "*.h" -and $f -notlike "*.frag") { continue }
    $d = git -c core.quotepath=false diff -U0 -- $f
    foreach ($l in $d) {
        if ($l -notlike "+*" -or $l -like "+++*") { continue }
        $added++
        $n = ([regex]::Matches($l.Substring(1), '"')).Count
        if ($n % 2 -ne 0) { $odd++ }
    }
}
Write-Output ("files={0} addedLines={1} oddQuoteLines={2}" -f $files.Count, $added, $odd)
# Self-check: read the banner line in main.cpp and count its quotes.
$all = [System.IO.File]::ReadAllLines("$Repo\src\poc-presenter\main.cpp", [System.Text.Encoding]::UTF8)
foreach ($l in $all) {
    if ($l -like "*poc-presenter v0.18.10*") {
        $n = ([regex]::Matches($l, '"')).Count
        Write-Output ("banner quotes={0} (expect 2)" -f $n)
    }
}
if ($odd -ne 0) { exit 1 }
exit 0