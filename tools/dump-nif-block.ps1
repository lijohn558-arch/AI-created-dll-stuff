param([string]$File, [string]$Want = 'BSTriShape', [int]$Count = 1, [int]$MaxHex = 176, [switch]$List, [int]$Idx = -1)

$ErrorActionPreference = 'Stop'
$b = [IO.File]::ReadAllBytes($File)
$p = 0
while ($b[$p] -ne 0x0A) { $p++ }
$p++
$p += 4                     # version
$p += 1                     # endian
$p += 4                     # user version
$numBlocks = [BitConverter]::ToUInt32($b, $p); $p += 4
$bsVer = [BitConverter]::ToUInt32($b, $p); $p += 4
$len = $b[$p]; $p += 1 + $len                      # author
if ($bsVer -lt 131) { $len = $b[$p]; $p += 1 + $len }   # process script
$len = $b[$p]; $p += 1 + $len                      # export script
if ($bsVer -ge 103) { $len = $b[$p]; $p += 1 + $len }
$numTypes = [BitConverter]::ToUInt16($b, $p); $p += 2
$types = New-Object string[] $numTypes
for ($i = 0; $i -lt $numTypes; $i++) {
    $l = [BitConverter]::ToInt32($b, $p); $p += 4
    $types[$i] = [Text.Encoding]::ASCII.GetString($b, $p, $l); $p += $l
}
$typeIdx = @()
for ($i = 0; $i -lt $numBlocks; $i++) { $typeIdx += [BitConverter]::ToUInt16($b, $p); $p += 2 }
$sizes = @()
$sum = [int64]0
for ($i = 0; $i -lt $numBlocks; $i++) { $s = [BitConverter]::ToUInt32($b, $p); $p += 4; $sizes += $s; $sum += $s }
$numStrings = [BitConverter]::ToUInt32($b, $p); $p += 4
$maxStrLen = [BitConverter]::ToUInt32($b, $p); $p += 4
for ($i = 0; $i -lt $numStrings; $i++) { $l = [BitConverter]::ToInt32($b, $p); $p += 4; $p += $l }
$groups = [BitConverter]::ToUInt32($b, $p); $p += 4
$blockStart = $p
Write-Output ("header: numBlocks={0} types={1} numStrings={2} maxStrLen={3} groups={4} blockStart={5} sum={6} fileLen={7} tail={8}" -f `
    $numBlocks, $numTypes, $numStrings, $maxStrLen, $groups, $blockStart, $sum, $b.Length, ($b.Length - $blockStart - $sum))
Write-Output ("blockTypes: {0}" -f ($types -join ', '))

$off = $blockStart
$shown = 0
for ($i = 0; $i -lt $numBlocks -and $shown -lt $Count; $i++) {
    if ($Idx -ge 0) {
        if ($i -ne $Idx) { $off += [int]$sizes[$i]; continue }
        $sz = [int]$sizes[$Idx]
        Write-Output ("--- block[{0}] type={1} offset={2} size={3}" -f $i, $types[$typeIdx[$i]], $off, $sz)
        $n = [Math]::Min($sz, $MaxHex)
        for ($k = 0; $k -lt $n; $k += 16) {
            $end = [Math]::Min($k + 15, $n - 1)
            $hex = ($k..$end | ForEach-Object { $b[$off + $_].ToString('X2') }) -join ' '
            Write-Output ("{0,4}: {1}" -f $k, $hex)
        }
        break
    }
    $t = $types[$typeIdx[$i]]
    $sz = [int]$sizes[$i]
    if ($t -eq $Want) {
        $shown++
        if ($List) {
            $tail = (($sz - 16)..($sz - 1) | ForEach-Object { $b[$off + $_].ToString('X2') }) -join ' '
            $mid = ''
            for ($k = 108; $k -lt [Math]::Min(132, $sz); $k += 4) {
                $mid += ('{0}:{1:X8} ' -f $k, [BitConverter]::ToUInt32($b, $off + $k))
            }
            Write-Output ("block[{0}] off={1} size={2} | after-desc: {3}| tail16: {4}" -f $i, $off, $sz, $mid, $tail)
        }
        else {
        Write-Output ("--- block[{0}] type={1} offset={2} size={3}" -f $i, $t, $off, $sz)
        $n = [Math]::Min($sz, $MaxHex)
        for ($k = 0; $k -lt $n; $k += 16) {
            $end = [Math]::Min($k + 15, $n - 1)
            $hex = ($k..$end | ForEach-Object { $b[$off + $_].ToString('X2') }) -join ' '
            $asc = ($k..$end | ForEach-Object { $c = $b[$off + $_]; if ($c -ge 32 -and $c -lt 127) { [char]$c } else { '.' } }) -join ''
            Write-Output ("{0,4}: {1,-47} {2}" -f $k, $hex, $asc)
        }
        }
    }
    $off += $sz
}
