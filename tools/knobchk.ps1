param(
    [string]$Key = 'ssr.v1det',
    [string]$Value = '',
    [string]$ForceLog = ''
)
# ASCII-only. Assert the LAST game startup actually read the knob you think you set.
# The ini is read ONCE at startup, so "I edited it" proves nothing -- the log echo does.
# exit 0 = PASS, 1 = FAIL (wrong/absent), 2 = no log found.
$ErrorActionPreference = 'Stop'

$cand = New-Object System.Collections.Generic.List[string]
if ($ForceLog -ne '' -and (Test-Path $ForceLog)) { $cand.Add($ForceLog) }
if (Test-Path 'D:\The Elder Scrolls V Skyrim\poc-presenter.log') {
    $cand.Add('D:\The Elder Scrolls V Skyrim\poc-presenter.log')
}
foreach ($g in @(Get-ChildItem -Path 'C:\Users\joker\AppData\Local\ModOrganizer\*\overwrite\SKSE\Plugins\poc-presenter.log' -ErrorAction SilentlyContinue)) {
    $cand.Add($g.FullName)
}
$logfile = ''; $newest = [DateTime]::MinValue
foreach ($g in $cand) {
    $t = (Get-Item $g).LastWriteTime
    if ($t -gt $newest) { $newest = $t; $logfile = $g }
}
if ($logfile -eq '') { Write-Output 'LOG NOT FOUND'; exit 2 }

$lines = [System.IO.File]::ReadAllLines($logfile, [System.Text.Encoding]::UTF8)
Write-Output ('log = ' + $logfile)
Write-Output ('mtime = ' + (Get-Item $logfile).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))

# last startup banner = the session you are about to judge
$last = -1
for ($i = 0; $i -lt $lines.Count; $i++) { if ($lines[$i] -match '==== poc-presenter v') { $last = $i } }
if ($last -lt 0) { Write-Output 'NO BANNER IN LOG'; exit 2 }
$ver = ''
if ($lines[$last] -match 'poc-presenter (v[0-9.]+)') { $ver = $matches[1] }
$stamp = ''
if ($lines[$last] -match '^\[([0-9 :-]+)\]') { $stamp = $matches[1] }
Write-Output ('last startup = ' + $stamp + '  ' + $ver + '  (line ' + $last + ')')

# scan forward from that banner for the knob echo.
# NOTE 1: the plugin echoes the short form (" v1det=0.300000"), while the ini key is "ssr.v1det"
#         => match EITHER the full key or the part after the last dot, or the gate always false-fails.
# NOTE 2: START AT $last+1. The banner line itself quotes old notes such as "14.30.5 debug=6",
#         and matching on the banner line made knobchk read that number instead of the real echo
#         (caught by pre-flight: -Key ssr.debug reported "used 6" from the banner, while the actual
#         echo further down said debug=0). The banner is exactly one logLine, so $last+1 is safe.
$full = [regex]::Escape($Key)
$short = $Key
$di = $Key.LastIndexOf('.')
if ($di -ge 0 -and $di -lt ($Key.Length - 1)) { $short = $Key.Substring($di + 1) }
$shortE = [regex]::Escape($short)
$rx = '(?:' + $full + '|' + $shortE + ')\s*=\s*([-0-9.eE+]+)'
$found = $false; $raw = ''; $at = -1
for ($i = $last + 1; $i -lt $lines.Count; $i++) {
    $m = [regex]::Match($lines[$i], $rx)
    if ($m.Success) { $found = $true; $raw = $m.Groups[1].Value; $at = $i; break }
}
if (-not $found) {
    Write-Output ('FAIL: ' + $Key + ' NOT echoed in the last startup block')
    Write-Output '      => this DLL does not read that key (wrong/stale DLL), or the knob is missing.'
    exit 1
}
Write-Output ('echoed ' + $Key + ' = ' + $raw + '   (line ' + $at + ')')

if ($Value -eq '') { Write-Output 'INFO: no -Value given, echo shown only.'; exit 0 }

$want = 0.0
try { $want = [double]::Parse($Value, [System.Globalization.CultureInfo]::InvariantCulture) }
catch { Write-Output ('BAD -Value: ' + $Value); exit 2 }
$got = 0.0
try { $got = [double]::Parse($raw, [System.Globalization.CultureInfo]::InvariantCulture) } catch { }
$ok = [Math]::Abs($got - $want) -lt 0.0001
Write-Output ('VERDICT: ' + $(if ($ok) { 'PASS - last run really used ' + $Key + '=' + $Value } else { 'FAIL - wanted ' + $Value + ' but last run used ' + $raw + '  => SHOT IS INVALID' }))
if ($ok) { exit 0 } else { exit 1 }
