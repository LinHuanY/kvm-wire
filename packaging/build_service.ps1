# 构建 SYSTEM 服务
# 用法: powershell -ExecutionPolicy Bypass -File packaging\build_service.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python -m PyInstaller `
    --noconfirm --clean --onedir --console `
    --name KVM-Wire-Service `
    --paths "$root\host" --paths "$root\service" `
    --hidden-import win32timezone --hidden-import servicemanager `
    --hidden-import win32service --hidden-import win32ts `
    --hidden-import win32serviceutil --hidden-import win32api `
    --hidden-import win32security --hidden-import win32process `
    --hidden-import win32event --hidden-import win32con `
    --hidden-import kvm_agent --hidden-import e2ecrypto --hidden-import wan_relay `
    --hidden-import engine --hidden-import hidmap --hidden-import wininject `
    --hidden-import kvmproto --hidden-import discovery `
    --distpath "$root\dist-service" --workpath "$root\build-service" `
    "$root\service\kvm_service.py"

if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed: $LASTEXITCODE" }
Write-Host "OK -> $root\dist-service\KVM-Wire-Service"
