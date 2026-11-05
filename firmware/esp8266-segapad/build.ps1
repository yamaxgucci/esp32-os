# Build the ESP8266 pad stand-in.  From PowerShell:  .\firmware\esp8266-segapad\build.ps1
#
# The SDK cannot build from a path with a space in it (see the RLINK firmware's
# README), so this copies the project to D:\sp8266fw with the shared pad code,
# builds there, and leaves the three images in D:\sp8266fw\build.
#
# Then it builds the application once more in D:\sp8266fw1, linked for the
# second slot (partitions_slot1.csv), for an update over Wi-Fi: ota.ps1 sends
# whichever of the two the module is not running.
param([string]$Work = 'D:\sp8266fw')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Resolve-Path (Join-Path $here '..\..')

$env:IDF_PATH = 'D:\Espressif\ESP8266_RTOS_SDK'
$env:IDF_TOOLS_PATH = 'D:\Espressif\tools8266'
$t = 'D:\Espressif\tools8266\tools'
$env:PATH = "$t\xtensa-lx106-elf\esp-2020r3-49-gd5524c1-8.4.0\xtensa-lx106-elf\bin;" +
            "$t\cmake\3.13.4\bin;$t\ninja\1.9.0;D:\dev\mingw64\bin;" +
            "D:\Espressif\py8266env\Scripts;$env:PATH"

function Build-In([string]$dir, [string]$table) {
    New-Item -ItemType Directory -Force (Join-Path $dir 'main') | Out-Null
    Copy-Item (Join-Path $here 'CMakeLists.txt'), (Join-Path $here 'partitions.csv'),
              (Join-Path $here 'partitions_slot1.csv') $dir -Force
    $cfg = Get-Content -Raw (Join-Path $here 'sdkconfig.defaults')
    $cfg = $cfg -replace 'CUSTOM_FILENAME="partitions.csv"', "CUSTOM_FILENAME=`"$table`""
    [IO.File]::WriteAllText((Join-Path $dir 'sdkconfig.defaults'), $cfg)
    Copy-Item (Join-Path $here 'main\*') (Join-Path $dir 'main') -Force
    Copy-Item (Join-Path $root 'apps\common\segapad\ag_segapad.*') (Join-Path $dir 'main') -Force
    # A copy keeps the source's time, and ninja does not count main/wifi_secret.h
    # appearing as a reason to compile again.
    (Get-Item (Join-Path $dir 'main\segapad_main.c')).LastWriteTime = Get-Date
    # sdkconfig is made from the defaults only when it is missing, so a stale
    # one would keep the other table
    $sdk = Join-Path $dir 'sdkconfig'
    if ((Test-Path $sdk) -and -not (Select-String -Quiet -SimpleMatch "`"$table`"" $sdk)) {
        Remove-Item $sdk
    }
    Push-Location $dir
    try {
        # through cmd: Windows PowerShell 5.1 turns the SDK's warnings on stderr
        # into errors and stops the build
        cmd /c "D:\Espressif\py8266env\Scripts\python.exe %IDF_PATH%\tools\idf.py build 2>&1"
        if ($LASTEXITCODE -ne 0) { throw "idf.py build in $dir failed ($LASTEXITCODE)" }
    } finally {
        Pop-Location
    }
}

Build-In $Work 'partitions.csv'
Build-In "${Work}1" 'partitions_slot1.csv'
'slot 0 (0x10000): ' + (Join-Path $Work 'build\segapad8266.bin')
'slot 1 (0x80000): ' + (Join-Path "${Work}1" 'build\segapad8266.bin')
