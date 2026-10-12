# enc_gate.ps1 -- ASCII-only encoding gate for docs 00/02/05: UTF-8 without BOM,
# pure LF (no CR), valid UTF-8. Exits 1 on any problem. Pure ASCII script.
$ErrorActionPreference = 'Stop'
$docs = 'C:\Users\joker\skyrim-vulkan\docs'
$strict = New-Object System.Text.UTF8Encoding($false, $true)
$fail = $false
foreach ($pat in @('00-*.md', '02-*.md', '05-*.md')) {
    $hit = @(Get-ChildItem -LiteralPath $docs -Filter $pat)
    if ($hit.Count -ne 1) { Write-Output ("ERR " + $pat + " count=" + $hit.Count); $fail = $true; continue }
    $b = [System.IO.File]::ReadAllBytes($hit[0].FullName)
    $bom = ($b.Length -ge 3 -and $b[0] -eq 0xEF -and $b[1] -eq 0xBB -and $b[2] -eq 0xBF)
    $lf = 0; $cr = 0
    for ($i = 0; $i -lt $b.Length; $i++) { if ($b[$i] -eq 0x0A) { $lf++; if ($i -gt 0 -and $b[$i-1] -eq 0x0D) { $cr++ } } }
    $u8 = $true
    try { [void]$strict.GetString($b) } catch { $u8 = $false }
    $ok = (-not $bom) -and ($cr -eq 0) -and $u8
    if (-not $ok) { $fail = $true }
    Write-Output ("{0,-34} {1,7} B  LF={2,5} CRLF={3,4} BOM={4} utf8={5}  {6}" -f $hit[0].Name, $b.Length, $lf, $cr, $bom, $u8, $(if ($ok) { "OK" } else { "FAIL" }))
}
if ($fail) { Write-Output "ENC GATE FAIL"; exit 1 }
Write-Output "ENC GATE PASS"
exit 0