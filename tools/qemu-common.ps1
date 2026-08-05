# Shared helpers for the QEMU scripts.  Dot-source, do not run directly.

function Resolve-Qemu {
    if (-not $env:IDF_TOOLS_PATH) {
        throw 'Run tools\idf-env.ps1 first.'
    }
    $exe = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'tools\qemu-xtensa') `
        -Filter 'qemu-system-xtensa.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $exe) {
        throw 'QEMU not installed. Run: idf_tools.py install qemu-xtensa'
    }
    return $exe.FullName
}

# Rebuilds build\qemu_flash.bin when the application binary is newer.
function Update-FlashImage {
    $app = 'build\argonos.bin'
    $flash = 'build\qemu_flash.bin'

    if (-not (Test-Path $app)) {
        throw "$app not found. Run idf.py build first."
    }
    if ((Test-Path $flash) -and
        (Get-Item $flash).LastWriteTime -ge (Get-Item $app).LastWriteTime) {
        return
    }

    $python = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
        -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
        Select-Object -First 1
    if (-not $python) { throw 'IDF python environment not found.' }

    Write-Host 'Generating build\qemu_flash.bin'
    & $python.FullName -m esptool --chip=esp32s3 merge_bin `
        --output=$flash --fill-flash-size=8MB `
        --flash_mode dio --flash_freq 80m --flash_size 8MB `
        0x0 build\bootloader\bootloader.bin `
        0x8000 build\partition_table\partition-table.bin `
        0x10000 $app | Out-Null
    if (-not $?) { throw 'esptool merge_bin failed.' }
}

function Initialize-EfuseFile {
    $efuse = 'build\qemu_efuse.bin'
    if (-not (Test-Path $efuse)) {
        # Blank efuses: everything default, nothing burned.
        [System.IO.File]::WriteAllBytes((Join-Path (Get-Location) $efuse),
                                        (New-Object byte[] 1024))
    }
    return $efuse
}

# Machine arguments common to every way we start the emulator.
function Get-QemuMachineArgs {
    param([string]$EfusePath)
    return @(
        '-M', 'esp32s3'
        '-m', '32M'
        '-drive', 'file=build\qemu_flash.bin,if=mtd,format=raw'
        '-drive', "file=$EfusePath,if=none,format=raw,id=efuse"
        '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse'
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true'
        '-global', 'driver=ssi_psram,property=is_octal,value=true'
    )
}
