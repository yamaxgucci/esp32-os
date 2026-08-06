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

# Rebuilds build\qemu_flash.bin when it does not match the current firmware.
#
# The freshness check is a recorded stamp rather than a comparison of file times,
# because the emulator writes to the flash image itself - the FAT on C: lives in
# there - so after any run the image looks newer than the firmware it was built
# from.  Comparing times then silently keeps testing the previous build, which is
# the worst kind of test result: a passing one, of the wrong thing.
#
# The image is kept rather than regenerated every time on purpose: C: is formatted
# on first boot, and throwing it away would add half a second to every boot and
# change the numbers the boot report is measured against.
function Update-FlashImage {
    $app = 'build\argonos.bin'
    $flash = 'build\qemu_flash.bin'
    $stamp = 'build\qemu_flash.stamp'

    if (-not (Test-Path $app)) {
        throw "$app not found. Run idf.py build first."
    }

    $want = "$((Get-Item $app).LastWriteTimeUtc.Ticks):$((Get-Item $app).Length)"
    if ((Test-Path $flash) -and (Test-Path $stamp) -and
        (Get-Content $stamp -Raw).Trim() -eq $want) {
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

    Set-Content -Path $stamp -Value $want -Encoding ascii
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
using System.Text;

public static class ArgonConsoleVt
{
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern IntPtr GetStdHandle(int nStdHandle);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetConsoleMode(IntPtr handle, out uint mode);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleMode(IntPtr handle, uint mode);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Auto)]
    static extern IntPtr CreateFile(string name, uint access, uint share,
                                    IntPtr security, uint disposition,
                                    uint flags, IntPtr template);

    [StructLayout(LayoutKind.Sequential)] struct COORD { public short X, Y; }
    [StructLayout(LayoutKind.Sequential)] struct SMALL_RECT { public short L, T, R, B; }
    [StructLayout(LayoutKind.Sequential)] struct CSBI {
        public COORD Size; public COORD Cursor; public ushort Attr;
        public SMALL_RECT Window; public COORD MaxSize;
    }

    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool GetConsoleScreenBufferInfo(IntPtr h, out CSBI info);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool WriteConsole(IntPtr h, string buf, uint len,
                                    out uint written, IntPtr reserved);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool ReadConsoleOutputCharacter(IntPtr h, StringBuilder buf,
                                                  uint len, COORD at,
                                                  out uint read);
    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    static extern bool FillConsoleOutputCharacter(IntPtr h, char c, uint len,
                                                  COORD at, out uint written);
    [DllImport("kernel32.dll", SetLastError = true)]
    static extern bool SetConsoleCursorPosition(IntPtr h, COORD at);

    const int STD_INPUT = -10;
    const int STD_OUTPUT = -11;

    const uint GENERIC_READ = 0x80000000;
    const uint GENERIC_WRITE = 0x40000000;
    const uint FILE_SHARE_RW = 0x00000003;
    const uint OPEN_EXISTING = 3;

    const uint ENABLE_PROCESSED_INPUT = 0x0001;
    const uint ENABLE_LINE_INPUT = 0x0002;
    const uint ENABLE_ECHO_INPUT = 0x0004;
    const uint ENABLE_VIRTUAL_TERMINAL_INPUT = 0x0200;
    const uint ENABLE_PROCESSED_OUTPUT = 0x0001;
    const uint ENABLE_VIRTUAL_TERMINAL_PROCESSING = 0x0004;

    public static uint SavedIn, SavedOut;
    public static bool HaveIn, HaveOut, VtEnabled;
    public static string Report = "";

    //
    // The console screen buffer is opened by name rather than taken from the
    // standard handle.  PowerShell's stdout is not always the console - a
    // pipeline anywhere in the call chain can replace it - and the mode has to
    // be set on the console itself, since that is what the child process will
    // inherit and write to.
    //
    static IntPtr OpenConsole(string name, uint access)
    {
        IntPtr h = CreateFile(name, access, FILE_SHARE_RW, IntPtr.Zero,
                              OPEN_EXISTING, 0, IntPtr.Zero);
        return h;
    }

    static bool Invalid(IntPtr h) { return h == IntPtr.Zero || h == new IntPtr(-1); }

    public static bool Configure()
    {
        string outcome = "";
        VtEnabled = false;

        // Output and input are handled independently: a failure on one must not
        // leave the other untouched, which is the bug this replaced.
        IntPtr o = OpenConsole("CONOUT$", GENERIC_READ | GENERIC_WRITE);
        if (Invalid(o)) {
            o = GetStdHandle(STD_OUTPUT);
        }
        uint om;
        if (!GetConsoleMode(o, out om)) {
            outcome = "no console on output (" + Marshal.GetLastWin32Error() + ")";
        } else {
            SavedOut = om;
            HaveOut = true;
            uint want = om | ENABLE_PROCESSED_OUTPUT |
                        ENABLE_VIRTUAL_TERMINAL_PROCESSING;
            if (!SetConsoleMode(o, want)) {
                outcome = "console refused escape sequences (" +
                          Marshal.GetLastWin32Error() + ")";
            } else {
                uint check;
                GetConsoleMode(o, out check);
                if ((check & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0) {
                    outcome = "escape sequences enabled";
                    VtEnabled = true;
                } else {
                    outcome = "console dropped the escape sequence flag";
                }
            }
        }

        IntPtr i = OpenConsole("CONIN$", GENERIC_READ | GENERIC_WRITE);
        if (Invalid(i)) {
            i = GetStdHandle(STD_INPUT);
        }
        uint im;
        if (GetConsoleMode(i, out im)) {
            SavedIn = im;
            HaveIn = true;
            // Virtual terminal input turns arrow keys into the escape sequences
            // the OS already decodes.  Line input and echo have to go, or the
            // console would buffer a whole line and print it twice.  Processed
            // input stays on deliberately so Ctrl+C can still break out of a
            // wedged emulator; the cost is that Ctrl+C does not reach the guest.
            uint want = (im | ENABLE_VIRTUAL_TERMINAL_INPUT |
                         ENABLE_PROCESSED_INPUT) &
                        ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT);
            if (!SetConsoleMode(i, want)) {
                outcome += ", keyboard left as it was";
            }
        } else {
            outcome += ", no console on input";
        }

        //
        // Trusting SetConsoleMode is what went wrong twice.  Write an escape
        // sequence, read the cell back, and believe the answer: if the console
        // interpreted it the cell is untouched, and if it did not the ESC is
        // sitting there in plain sight.
        //
        Report = outcome;
        return VtEnabled;
    }

    //
    // An end-to-end check: write an escape sequence, read the cell back, and see
    // whether the console acted on it or printed it.  Not used when starting the
    // emulator - the mode's return value answers that question, and writing to
    // somebody's terminal to double-check it is not free.  This is what the vt
    // diagnostic runs when asked.
    //
    public static bool VerifyRendering()
    {
        IntPtr o = OpenConsole("CONOUT$", GENERIC_READ | GENERIC_WRITE);
        if (Invalid(o)) { return false; }
        return Interprets(o);
    }

    static bool Interprets(IntPtr o)
    {
        CSBI before;
        if (!GetConsoleScreenBufferInfo(o, out before)) {
            return false;
        }

        // Erase-to-end-of-line: nothing to see if it works, three visible
        // characters if it does not.
        string probe = "\u001b[K";
        uint written;
        if (!WriteConsole(o, probe, (uint)probe.Length, out written, IntPtr.Zero)) {
            return false;
        }

        StringBuilder cell = new StringBuilder(4);
        uint read;
        if (!ReadConsoleOutputCharacter(o, cell, 1, before.Cursor, out read) ||
            read == 0) {
            return false;
        }

        bool literal = cell[0] == '\u001b';
        if (literal) {
            // Wipe the evidence and put the cursor back where it was.
            uint cleared;
            FillConsoleOutputCharacter(o, ' ', (uint)probe.Length,
                                       before.Cursor, out cleared);
            SetConsoleCursorPosition(o, before.Cursor);
        }
        return !literal;
    }

    public static void Restore()
    {
        if (HaveOut) {
            IntPtr o = OpenConsole("CONOUT$", GENERIC_READ | GENERIC_WRITE);
            if (Invalid(o)) { o = GetStdHandle(STD_OUTPUT); }
            SetConsoleMode(o, SavedOut);
        }
        if (HaveIn) {
            IntPtr i = OpenConsole("CONIN$", GENERIC_READ | GENERIC_WRITE);
            if (Invalid(i)) { i = GetStdHandle(STD_INPUT); }
            SetConsoleMode(i, SavedIn);
        }
    }
}
'@

function Enable-ConsoleVt { return [ArgonConsoleVt]::Configure() }
function Get-ConsoleVtReport { return [ArgonConsoleVt]::Report }
function Test-ConsoleRendering { return [ArgonConsoleVt]::VerifyRendering() }
function Restore-ConsoleVt { [ArgonConsoleVt]::Restore() }

# Attaches a card image, creating a blank one if it is missing.  A blank image
# will not mount, by design: ArgonOS never formats removable media on its own.
# Use the format command in the shell to make a filesystem on it.
function Get-QemuSdArgs {
    param([string]$Path = 'build\sdcard.img', [int]$SizeMB = 64)

    if (-not (Test-Path $Path)) {
        New-Item -ItemType Directory -Force -Path (Split-Path -Parent $Path) |
            Out-Null
        $file = [System.IO.File]::Create((Join-Path (Get-Location) $Path))
        $file.SetLength([long]$SizeMB * 1MB)
        $file.Close()
        Write-Host "Created blank $SizeMB MB card image $Path"
    }
    return @('-drive', "file=$Path,if=sd,format=raw")
}

# Machine arguments common to every way we start the emulator.
function Get-QemuMachineArgs {
    param([string]$EfusePath)
    return @(
        '-M', 'esp32s3'
        # 8 MB, matching an N16R8 module.  Not a detail: the S3 reaches flash
        # and PSRAM through one 32 MB window on the data bus, so 32 MB of PSRAM
        # leaves no address space to map flash into, and esp_partition - which
        # reads the partition table through a mapping rather than a read - then
        # finds no partitions at all.
        '-m', '8M'
        '-drive', 'file=build\qemu_flash.bin,if=mtd,format=raw'
        '-drive', "file=$EfusePath,if=none,format=raw,id=efuse"
        '-global', 'driver=nvram.esp32s3.efuse,property=drive,value=efuse'
        '-global', 'driver=timer.esp32s3.timg,property=wdt_disable,value=true'
        '-global', 'driver=ssi_psram,property=is_octal,value=true'
    )
}
