# 安装 KVM-Wire SYSTEM 服务（需管理员）
# 作用：在锁屏 / UAC 的 Secure Desktop 上也能注入键鼠
# 用法（管理员 PowerShell）：
#   powershell -ExecutionPolicy Bypass -File packaging\install_service.ps1
# 可选参数：-RelayHost -RelayPort -ClientKey -PairCode -CaFile
param(
    [string]$RelayHost = "",
    [int]$RelayPort = 8790,
    [string]$ClientKey = "",
    [string]$PairCode = "",
    [string]$CaFile = ""
)

$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "请右键“以管理员身份运行”PowerShell，再执行此脚本"
}

$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path "$env:ProgramData\KVM-Wire" | Out-Null
Start-Transcript -Path "$env:ProgramData\KVM-Wire\install.log" -Force | Out-Null
$srcDir = Join-Path $root "dist-service\KVM-Wire-Service"
if (-not (Test-Path $srcDir)) {
    throw "找不到 $srcDir，请先运行 packaging\build_service.ps1"
}

# 优先复用 GUI 的 kvm-wire.json
$guiCfgPath = Join-Path $env:LOCALAPPDATA "Programs\KVM-Wire\kvm-wire.json"
$gui = $null
if (Test-Path $guiCfgPath) {
    $gui = Get-Content $guiCfgPath -Raw -Encoding UTF8 | ConvertFrom-Json
}
if (-not $RelayHost) { $RelayHost = $gui.relay_host }
if (-not $ClientKey) { $ClientKey = $gui.relay_key }
if (-not $PairCode) { $PairCode = $gui.pair_code }
if (-not $CaFile) { $CaFile = $gui.ca }
if (-not $RelayHost -or -not $ClientKey -or -not $PairCode) {
    throw "缺少中继地址/客户端密钥/配对码，请传参数或先配置 GUI 的 kvm-wire.json"
}

$installDir = Join-Path $env:ProgramFiles "KVM-Wire-Service"
$dataDir = Join-Path $env:ProgramData "KVM-Wire"
New-Item -ItemType Directory -Force -Path $installDir, $dataDir | Out-Null
Copy-Item (Join-Path $srcDir "*") $installDir -Recurse -Force

$caDest = Join-Path $dataDir "ca.crt"
if ($CaFile -and (Test-Path $CaFile)) {
    Copy-Item $CaFile $caDest -Force
} else {
    throw "找不到 CA 证书：$CaFile"
}

$cfg = [ordered]@{
    relay_host = $RelayHost
    relay_port = $RelayPort
    relay_key  = $ClientKey
    pair_code  = $PairCode
    ca         = $caDest
    device_id  = ""
}
[IO.File]::WriteAllText((Join-Path $dataDir "service.json"), ($cfg | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

# 限制配置权限：SYSTEM / Administrators
icacls $dataDir /inheritance:r /grant:r "*S-1-5-18:(OI)(CI)F" "*S-1-5-32-544:(OI)(CI)F" | Out-Null

Get-Process KVM-Wire -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
$svcExe = Join-Path $installDir "KVM-Wire-Service.exe"
& $svcExe stop 2>$null | Out-Null
& $svcExe remove 2>$null | Out-Null
& $svcExe install
& $svcExe start
sc.exe config KVMWireSecure start= auto | Out-Null
Write-Host "已安装并启动 KVMWireSecure（SYSTEM）"
Write-Host "日志：$dataDir\service.log"
Write-Host "卸载：packaging\uninstall_service.ps1"


