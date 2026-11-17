# 卸载 KVM-Wire SYSTEM 服务（需管理员）
$ErrorActionPreference = "Stop"
$principal = [Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw "请右键“以管理员身份运行”PowerShell"
}

$installDir = Join-Path $env:ProgramFiles "KVM-Wire-Service"
$svcExe = Join-Path $installDir "KVM-Wire-Service.exe"
if (Test-Path $svcExe) {
    & $svcExe stop 2>$null | Out-Null
    & $svcExe remove 2>$null | Out-Null
}
if (Test-Path $installDir) { Remove-Item $installDir -Recurse -Force }
Write-Host "服务已卸载（配置保留在 $env:ProgramData\KVM-Wire）"
