# Build the ESP8266 pad stand-in.  From PowerShell:  .\firmware\esp8266-segapad\build.ps1
#
# The SDK cannot build from a path with a space in it (see the RLINK firmware's
# README), so this copies the project to D:\sp8266fw with the shared pad code,
# builds there, and leaves the three images in D:\sp8266fw\build.
param([string]$Work = 'D:\sp8266fw')
$ErrorActionPreference = 'Stop'
$here = $PSScriptRoot
$root = Resolve-Path (Join-Path $here '..\..')

New-Item -ItemType Directory -Force (Join-Path $Work 'main') | Out-Null
Copy-Item (Join-Path $here 'CMakeLists.txt'), (Join-Path $here 'sdkconfig.defaults'), (Join-Path $here 'partitions.csv') $Work -Force
Copy-Item (Join-Path $here 'main\*') (Join-Path $Work 'main') -Force
Copy-Item (Join-Path $root 'apps\common\segapad\ag_segapad.*') (Join-Path $Work 'main') -Force
# A copy keeps the source's time, and ninja does not count main/wifi_secret.h
# appearing as a reason to compile again.
(Get-Item (Join-Path $Work 'main\segapad_main.c')).LastWriteTime = Get-Date

$env:IDF_PATH = 'D:\Espressif\ESP8266_RTOS_SDK'
$env:IDF_TOOLS_PATH = 'D:\Espressif\tools8266'
$t = 'D:\Espressif\tools8266\tools'
$env:PATH = "$t\xtensa-lx106-elf\esp-2020r3-49-gd5524c1-8.4.0\xtensa-lx106-elf\bin;" +
            "$t\cmake\3.13.4\bin;$t\ninja\1.9.0;D:\dev\mingw64\bin;" +
            "D:\Espressif\py8266env\Scripts;$env:PATH"
Push-Location $Work
try {
    # through cmd: Windows PowerShell 5.1 turns the SDK's warnings on stderr
    # into errors and stops the build
    cmd /c "D:\Espressif\py8266env\Scripts\python.exe %IDF_PATH%\tools\idf.py build 2>&1"
    if ($LASTEXITCODE -ne 0) { throw "idf.py build failed ($LASTEXITCODE)" }
} finally {
    Pop-Location
}
