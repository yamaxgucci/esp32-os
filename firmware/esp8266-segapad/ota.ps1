# Update the pad stand-in over Wi-Fi, no jumper.  From PowerShell, after build.ps1:
#   .\firmware\esp8266-segapad\ota.ps1 [-Module 192.168.0.126]
#
# On a 1 MB ESP8266 an image runs only from the slot it was linked for, so this
# asks the module which slot it runs (GET /ota) and sends the image built for
# the other.  A module from before GET /ota answers 405; it was flashed by wire
# at 0x10000 and runs slot 0.  Then it checks that the module came back in the
# other slot.
param([string]$Module = '192.168.0.126', [string]$Work = 'D:\sp8266fw')
$ErrorActionPreference = 'Stop'

function Slot {
    $a = (& curl.exe -s -m 5 -w ' %{http_code}' "http://$Module/ota") -join ' '
    if ($a -match '^slot (\d)') { return [int]$Matches[1] }
    if ($a -match ' 40[45]$') { return 0 }
    return -1
}

$was = Slot
if ($was -lt 0) { throw "no answer from http://$Module/ota" }
$bin = if ($was -eq 0) { Join-Path "${Work}1" 'build\segapad8266.bin' } else { Join-Path $Work 'build\segapad8266.bin' }
"module runs slot $was; sending $bin ($((Get-Item $bin).Length) bytes)"
& curl.exe -s -m 120 --data-binary "@$bin" -H 'Content-Type: application/octet-stream' "http://$Module/ota"
if ($LASTEXITCODE -ne 0) { throw "upload failed (curl $LASTEXITCODE)" }
for ($i = 0; $i -lt 30; $i++) {
    Start-Sleep 1
    $now = Slot
    if ($now -ge 0) { break }
}
if ($now -eq 1 - $was) { "module back, running slot $now" } else { throw "module answers slot $now, wanted $(1 - $was)" }
