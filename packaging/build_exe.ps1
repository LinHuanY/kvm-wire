# 打包 PC 端为单文件 exe（Windows）
# 用法: powershell -ExecutionPolicy Bypass -File packaging\build_exe.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root

python -m PyInstaller `
    --noconfirm --clean --onefile --windowed `
    --name KVM-Wire `
    --icon "$root\assets\icon.ico" `
    --paths "$root\host" `
    --distpath "$root\dist" `
    --workpath "$root\packaging\build" `
    "$root\host\kvm_gui.py"

if ($LASTEXITCODE -ne 0) { throw "PyInstaller failed: $LASTEXITCODE" }
Write-Host "OK -> $root\dist\KVM-Wire.exe"
