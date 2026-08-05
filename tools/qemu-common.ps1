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

# The classic Windows console prints escape sequences literally unless asked
# not to.  QEMU does not ask, so the boot output arrives as a wall of "<-[2J"
# and the screen never appears.  Windows Terminal and PowerShell 7 enable this
# themselves; cmd.exe and powershell.exe do not.
#
# Returns the previous modes so the caller can put the console back: leaving
# echo disabled would make the user's shell look broken after we exit.
Add-Type -ErrorAction SilentlyContinue -TypeDefinition @'
using System;
using System.Runtime.InteropServices;

public static class ArgonConsoleVt
{
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int nStdHandle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetConsoleMode(IntPtr handle, out uint mode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleMode(IntPtr handle, uint mode);

    const int STD_INPUT = -10;
    const int STD_OUTPUT = -11;

    const uint ENABLE_PROCESSED_INPUT = 0x0001;
    const uint ENABLE_LINE_INPUT = 0x0002;
    const uint ENABLE_ECHO_INPUT = 0x0004;
    const uint ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200;
    const uint ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004;

    public static uint SavedIn = 0;
    public static uint SavedOut = 0;
    public static bool Saved = false;

    public static bool Enable()
    {
        IntPtr o = GetStdHandle(STD_OUTPUT);
        IntPtr i = GetStdHandle(STD_INPUT);
        uint om, im;

        if (!GetConsoleMode(o, out om) || !GetConsoleMode(i, out im)) {
            return false;   // not a real console, e.g. output is redirected
        }

        SavedOut = om;
        SavedIn = im;
        Saved = true;

        if (!SetConsoleMode(o, om | ENABLE_VIRTUAL_TERMINAL_PROCESSING)) {
            return false;
        }

        // Virtual terminal input turns arrow keys into the escape sequences the
        // OS already knows how to decode.  Line input and echo have to go or
        // the console would buffer a whole line and print it twice.  Processed
        // input stays on deliberately, so Ctrl+C still breaks out of a wedged
        // emulator; the cost is that Ctrl+C does not reach the guest.
        im |= ENABLE_VIRTUAL_TERMINAL_INPUT;
        im &= ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
        im |= ENABLE_PROCESSED_INPUT;
        SetConsoleMode(i, im);
        return true;
    }

    public static void Restore()
    {
        if (!Saved) { return; }
        SetConsoleMode(GetStdHandle(STD_OUTPUT), SavedOut);
        SetConsoleMode(GetStdHandle(STD_INPUT), SavedIn);
    }
}
'@

function Enable-ConsoleVt { return [ArgonConsoleVt]::Enable() }
function Restore-ConsoleVt { [ArgonConsoleVt]::Restore() }

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
