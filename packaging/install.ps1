# 安装到当前用户目录，并在开始菜单创建快捷方式
# 用法: powershell -ExecutionPolicy Bypass -File packaging\install.ps1
$ErrorActionPreference = "Stop"
$root = Split-Path $PSScriptRoot -Parent
$exe = Join-Path $root "dist\KVM-Wire.exe"
if (-not (Test-Path $exe)) { throw "先运行 packaging\build_exe.ps1 生成 $exe" }

$installDir = Join-Path $env:LOCALAPPDATA "Programs\KVM-Wire"
New-Item -ItemType Directory -Force -Path $installDir | Out-Null
Copy-Item $exe $installDir -Force
Copy-Item (Join-Path $root "assets\icon.ico") $installDir -Force
$caSrc = Join-Path $root "server\ca.crt"
if (Test-Path $caSrc) { Copy-Item $caSrc $installDir -Force }

$target = Join-Path $installDir "KVM-Wire.exe"
$startMenu = [Environment]::GetFolderPath("Programs")
$lnk = Join-Path $startMenu "KVM-Wire.lnk"

$shell = New-Object -ComObject WScript.Shell
$sc = $shell.CreateShortcut($lnk)
$sc.TargetPath = $target
$sc.WorkingDirectory = $installDir
$sc.IconLocation = "$target,0"
$sc.Description = "KVM-Wire - 蓝牙键鼠网络 KVM"
$sc.Save()

$desktop = [Environment]::GetFolderPath("Desktop")
$sc2 = $shell.CreateShortcut((Join-Path $desktop "KVM-Wire.lnk"))
$sc2.TargetPath = $target
$sc2.WorkingDirectory = $installDir
$sc2.IconLocation = "$target,0"
$sc2.Description = "KVM-Wire - 蓝牙键鼠网络 KVM"
$sc2.Save()

Write-Host "已安装: $target"
Write-Host "开始菜单: $lnk"

