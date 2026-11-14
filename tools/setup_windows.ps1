# One-time laptop setup: a virtual environment (.venv) with the dev tools (PlatformIO, esptool,
# pyserial) and the gateway (for local testing). Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
$ErrorActionPreference = "Stop"

# Find a real Python 3.11+ (the Microsoft Store "python" stub does not count).
$candidates = @(@("py", "-3"), @("python"))
$py = $null
foreach ($c in $candidates) {
    try {
        $exe = $c[0]; $extra = $c[1..($c.Length - 1)]
        $v = & $exe @extra -c "import sys; print('%d.%d' % sys.version_info[:2])" 2>$null
        if ($LASTEXITCODE -eq 0 -and [version]$v -ge [version]"3.11") { $py = $c; break }
    } catch {}
}
if (-not $py) { throw "Python 3.11+ not found. Install it from python.org (tick 'Add python.exe to PATH') and re-run." }
$exe = $py[0]; $extra = $py[1..($py.Length - 1)]
Write-Host "Using Python $(& $exe @extra --version)"

if (-not (Test-Path .venv)) { & $exe @extra -m venv .venv }
.\.venv\Scripts\python.exe -m pip install --upgrade pip
.\.venv\Scripts\python.exe -m pip install -r tools\requirements.txt -r gateway\requirements.txt

Write-Host ""
Write-Host "Done. Next (no activation needed):"
Write-Host "  .\.venv\Scripts\python.exe tools\verify_hardware.py --list-ports"
Write-Host "  .\.venv\Scripts\python.exe tools\verify_hardware.py"
