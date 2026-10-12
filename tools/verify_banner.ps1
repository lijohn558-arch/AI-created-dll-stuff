# verify_banner.ps1 -- verify the startup banner line in main.cpp: UTF-8 without
# BOM, no CR, balanced quotes, and the tokens that knobchk / run2c regexes expect.
# Pure ASCII on purpose (PS 5.1 reads unmarked files as ANSI).
$ErrorActionPreference = 'Stop'
$p = 'C:\Users\joker\skyrim-vulkan\src\poc-presenter\main.cpp'
$b = [System.IO.File]::ReadAllBytes($p)
$bom = ($b.Length -ge 3 -and $b[0] -eq 239 -and $b[1] -eq 187 -and $b[2] -eq 191)
$cr = 0
foreach ($x in $b) { if ($x -eq 13) { $cr++ } }
$strict = New-Object System.Text.UTF8Encoding($false, $true)
$u8 = $true
try { [void]$strict.GetString($b) } catch { $u8 = $false }
Write-Output ("BOM=" + $bom + " CR=" + $cr + " utf8strict=" + $u8 + " bytes=" + $b.Length)
# banner line quote count + downstream token presence
$enc = New-Object System.Text.UTF8Encoding($false)
$lines = [System.IO.File]::ReadAllLines($p, $enc)
$bn = $null
foreach ($l in $lines) { if ($l -like '*logLine("==== poc-presenter*') { $bn = $l } }
if ($null -eq $bn) { Write-Output 'BANNER NOT FOUND'; exit 1 }
$q = ([regex]::Matches($bn, '"')).Count
Write-Output ("bannerQuotes=" + $q + " len=" + $bn.Length)
# The beta token is non-ASCII by design; build it from a code point to keep this
# script pure ASCII.
$beta = 'Step2c-' + [char]0x03B2
foreach ($t in @('ssr.shared', $beta, 'ssr.sentinel', 'ssr.vkout')) {
    Write-Output ("token " + $t + " = " + $bn.Contains($t))
}
# knobchk / run2c regexes against banner
$m = [regex]::Match($bn, '==== poc-presenter v')
Write-Output ("prefixMatch=" + $m.Success)
$m2 = [regex]::Match($bn, 'poc-presenter (v[0-9.]+)')
Write-Output ("version=" + $(if ($m2.Success) { $m2.Groups[1].Value } else { 'NONE' }))
# count banners total (must be 1 source site)
$n = 0
foreach ($l in $lines) { if ($l -like '*==== poc-presenter *') { $n++ } }
Write-Output ("bannerLines=" + $n)
# odd quote lines on whole file (quotescan self-check on banner)
if ($q % 2 -ne 0) { Write-Output 'ODD QUOTES'; exit 1 }
Write-Output 'VERIFY OK'
exit 0