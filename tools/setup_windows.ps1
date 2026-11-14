# One-time laptop setup: a virtual environment with the dev tools (PlatformIO, esptool,
# pyserial) and the gateway (for local testing). Run from the repository root:
#   powershell -ExecutionPolicy Bypass -File tools\setup_windows.ps1
$ErrorActionPreference = "Stop"
$py = (Get-Command python -ErrorAction SilentlyContinue)
if (-not $py) { throw "Python not found. Install Python 3.11+ from python.org and tick 'Add to PATH'." }
python --version
if (-not (Test-Path .venv)) { python -m venv .venv }
.\.venv\Scripts\python -m pip install --upgrade pip
.\.venv\Scripts\python -m pip install -r tools\requirements.txt -r gateway\requirements.txt
Write-Host ""
Write-Host "Done. Activate with:  .\.venv\Scripts\Activate.ps1"
Write-Host "Then plug in the board and run:  python tools\verify_hardware.py"
