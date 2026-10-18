# start_server.ps1 - one-click local pose server for fit-coach device testing.
#
# Keep this file PURE ASCII (PowerShell 5.1 GBK pitfall, see build_fc.ps1).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File server\start_server.ps1          # real MediaPipe
#   powershell -ExecutionPolicy Bypass -File server\start_server.ps1 -Mock    # sine mock, no mediapipe needed
#
# The venv is created automatically on first run (server\venv). The device
# provisioning portal should point at http://<this PC's LAN IP>:8000.
param(
    [switch]$Mock,
    [int]$Port = 8000
)

$ErrorActionPreference = "Stop"
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$venvPython = Join-Path $here "venv\Scripts\python.exe"

if (-not (Test-Path $venvPython)) {
    Write-Host "[srv] creating venv (first run) ..."
    python -m venv (Join-Path $here "venv")
    & $venvPython -m pip install --quiet --upgrade pip
    & $venvPython -m pip install -r (Join-Path $here "requirements.txt")
}

# LAN IPv4 for the provisioning portal.
$lan = (Get-NetIPAddress -AddressFamily IPv4 |
    Where-Object { $_.IPAddress -notlike "127.*" -and $_.IPAddress -notlike "169.254.*" } |
    Select-Object -First 1).IPAddress
if (-not $lan) { $lan = "<LAN IP>" }

Write-Host ""
Write-Host "============================================================"
if ($Mock) { $env:FC_MOCK = "1" } else { Remove-Item Env:FC_MOCK -ErrorAction SilentlyContinue }
Write-Host (" fit-coach pose server  mode=" + $(if ($Mock) {"MOCK"} else {"MediaPipe"}))
Write-Host (" Device portal address : http://" + $lan + ":" + $Port)
Write-Host "------------------------------------------------------------"
Write-Host " If the device cannot reach this PC, allow the port once in"
Write-Host " an ADMIN PowerShell:"
Write-Host ("   netsh advfirewall firewall add rule name=`"fit-coach " + $Port + "`" dir=in action=allow protocol=TCP localport=" + $Port)
Write-Host "============================================================"
Write-Host ""

Set-Location $here
& $venvPython -m uvicorn pose_server:app --host 0.0.0.0 --port $Port
