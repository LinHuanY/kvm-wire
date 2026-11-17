# SYSTEM 服务：锁屏 / UAC 下也能注入

普通用户进程的 `SendInput` 只能注入当前用户桌面；Windows 锁屏、登录界面、UAC
弹窗位于 **Secure Desktop（Winlogon）**，会被系统直接丢弃。

KVM-Wire 提供一个可选的 **Windows 服务**：

- 以 `LocalSystem` 运行
- 在活动控制台会话中创建 SYSTEM 权限的助手进程
- 助手跟随当前输入桌面（Default / Winlogon），再调用 `SendInput`
- 因此锁屏状态下也能输入密码、操作 UAC

## 安装（需要管理员）

1. 先构建服务：

```powershell
powershell -ExecutionPolicy Bypass -File packaging\build_service.ps1
```

2. 右键「以管理员身份运行」PowerShell，执行：

```powershell
powershell -ExecutionPolicy Bypass -File packaging\install_service.ps1
```

脚本会：

- 复用 `%LOCALAPPDATA%\Programs\KVM-Wire\kvm-wire.json` 里的中继配置
- 复制程序到 `%ProgramFiles%\KVM-Wire-Service`
- 写配置到 `%ProgramData%\KVM-Wire\service.json`（仅 SYSTEM/管理员可读）
- 安装并启动服务 `KVMWireSecure`

也可以显式传参数：

```powershell
powershell -ExecutionPolicy Bypass -File packaging\install_service.ps1 `
  -RelayHost relay.example.com -RelayPort 8790 `
  -ClientKey <client_key> -PairCode <pair_code> -CaFile .\ca.crt
```

## 使用

- 服务安装后，**不要再用 GUI 连接中继**（两者会抢同一个设备）
- 关掉 KVM-Wire 窗口即可；服务会常驻
- 锁屏后直接用蓝牙键鼠输入
- 日志：`C:\ProgramData\KVM-Wire\service.log`

## 卸载

```powershell
powershell -ExecutionPolicy Bypass -File packaging\uninstall_service.ps1
```

## 安全说明

- 服务以 SYSTEM 运行，能模拟任意输入，权限很高；只在自己控制的机器上安装
- 配置目录 ACL 限制为 SYSTEM + Administrators
- 中继密钥/配对码仍走端到端加密，但服务本机可解密
- 如不需要锁屏注入，建议不安装服务，继续用普通 GUI

## 限制

- 只支持 Windows 10/11 x64
- 服务启动时需要用户已登录（活动控制台会话）
- 切换用户/注销后服务会自动为新会话重启助手
- 仍不支持 BIOS/POST（系统未启动时）
