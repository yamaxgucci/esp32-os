# Sets up the ESP-IDF environment for building ArgonOS in the current shell.
#
#   . .\tools\idf-env.ps1      # note the leading dot: it must run in-process
#   idf.py build
#
# Machine specific paths are not committed.  Provide them either through the
# environment (ARGON_IDF_PATH, ARGON_IDF_TOOLS_PATH, ARGON_HOST_CC_BIN) or in
# tools\local-env.ps1, which is gitignored.

$local = Join-Path $PSScriptRoot 'local-env.ps1'
if (Test-Path $local) { . $local }

if (-not $env:ARGON_IDF_PATH) {
    foreach ($c in @('D:\Espressif\esp-idf', 'C:\Espressif\esp-idf',
                     "$env:USERPROFILE\esp\esp-idf")) {
        if (Test-Path $c) { $env:ARGON_IDF_PATH = $c; break }
    }
}
if (-not $env:ARGON_IDF_TOOLS_PATH) {
    foreach ($c in @('D:\Espressif\tools', 'C:\Espressif',
                     "$env:USERPROFILE\.espressif")) {
        if (Test-Path $c) { $env:ARGON_IDF_TOOLS_PATH = $c; break }
    }
}

if (-not $env:ARGON_IDF_PATH -or -not (Test-Path $env:ARGON_IDF_PATH)) {
    Write-Error "ESP-IDF not found. Set ARGON_IDF_PATH or create tools\local-env.ps1."
    return
}

$env:IDF_PATH = $env:ARGON_IDF_PATH
$env:IDF_TOOLS_PATH = $env:ARGON_IDF_TOOLS_PATH

# idf_tools.py (and later idf.py) write temp files under %TEMP%.  On this
# machine that is often a small C: volume; export then dies with exit 120 /
# "No space left on device".  Keep the session temp next to the IDF tools
# (D:, no spaces) instead of the user profile.
$argonTmp = Join-Path $env:IDF_TOOLS_PATH 'tmp'
New-Item -ItemType Directory -Force -Path $argonTmp | Out-Null
$env:TEMP = $argonTmp
$env:TMP = $argonTmp

# idf_tools.py needs a Python to run itself; the one inside the IDF virtual
# environment is the right choice once it exists.  ARGON_PYTHON is for host
# tools (mkfatimg, virt.py) and often lacks the IDF venv packages — using it
# here makes `export` fail with a bare exit 120.
$venvPython = Get-ChildItem -Path (Join-Path $env:IDF_TOOLS_PATH 'python_env') `
    -Filter 'python.exe' -Recurse -ErrorAction SilentlyContinue |
    Select-Object -First 1
$python = if ($venvPython) { $venvPython.FullName }
          elseif ($env:ARGON_PYTHON) { $env:ARGON_PYTHON }
          else { 'python' }

# stderr from idf_tools ("Not using an unsupported version of cmake…") must not
# become a terminating error when the caller has $ErrorActionPreference=Stop.
# Redirect through a temp file so native stderr is never a PowerShell error.
$exportPy = Join-Path $env:IDF_PATH 'tools\idf_tools.py'
$exportTxt = Join-Path $env:TEMP ("argon-idf-export-{0}.txt" -f $PID)
$exportErr = Join-Path $env:TEMP ("argon-idf-export-{0}.err" -f $PID)
$p = Start-Process -FilePath $python -ArgumentList @(
        $exportPy, 'export', '--format', 'key-value'
    ) -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $exportTxt -RedirectStandardError $exportErr
if ($p.ExitCode -ne 0) {
    $detail = ''
    if (Test-Path -LiteralPath $exportErr) {
        $detail = (Get-Content -LiteralPath $exportErr -Raw -ErrorAction SilentlyContinue)
    }
    if (-not $detail -and (Test-Path -LiteralPath $exportTxt)) {
        $detail = (Get-Content -LiteralPath $exportTxt -Raw -ErrorAction SilentlyContinue)
    }
    if (-not $detail) { $detail = '' }
    Write-Error ("idf_tools.py export failed (exit {0}) using {1}.{2}{3}" -f `
        $p.ExitCode, $python, [Environment]::NewLine, $detail.Trim())
    return
}
foreach ($line in Get-Content -LiteralPath $exportTxt) {
    if ($line -match '^([A-Za-z_][A-Za-z0-9_]*)=(.*)$') {
        $name = $Matches[1]
        $value = $Matches[2].Replace('%PATH%', $env:PATH)
        [Environment]::SetEnvironmentVariable($name, $value, 'Process')
    }
}
Remove-Item -LiteralPath $exportTxt, $exportErr -Force -ErrorAction SilentlyContinue

# The host unit tests need a native compiler, which ESP-IDF does not provide.
if ($env:ARGON_HOST_CC_BIN -and (Test-Path $env:ARGON_HOST_CC_BIN)) {
    $env:Path = "$env:ARGON_HOST_CC_BIN;$env:Path"
}

Write-Host "IDF_PATH       = $env:IDF_PATH"
Write-Host "IDF_TOOLS_PATH = $env:IDF_TOOLS_PATH"
Write-Host "TEMP           = $env:TEMP"
