# KVM-Wire

把蓝牙键鼠变成网络 KVM：ESP32-S3 作为 BLE HID Host 连接蓝牙键盘/鼠标，通过 WiFi
把输入转发给 PC，PC 端注入为真实键鼠。支持**局域网直连**和**广域网 TLS 中继**。

> 面向“台式机不方便带走，只能带平板 + 键鼠出差”的专业用户：
> 键鼠绕过平板直连 ESP32，平板只负责看画面，**IDE/CAD/终端里的组合键和快捷键不再被平板吃掉**。
>
> 详细背景、痛点和方案对比见 [docs/INTRODUCTION.md](docs/INTRODUCTION.md)。

---

## 特性

- **BLE HID Host**：ESP32-S3 直接连接 BLE 键盘/鼠标（Bluedroid + esp_hid）
- **免改代码配网**：设备开一个开放热点 `KVM-Setup-XXXX`，连上自动弹出配置页；网页里填 WiFi、连键鼠、配中继
- **局域网自动发现**：PC 端 UDP 广播发现设备，点击确认即可连接
- **广域网中继**：设备和 PC 都主动连 TLS 中继，按 device_id 配对，穿透 NAT
- **Windows 注入**：`SendInput` + 绝对坐标跟踪，支持多显示器、滚轮、横向滚动、按键重复
- **自动重连**：键鼠休眠断开后按名字扫描重连，不用每次开网页
- **低延迟**：TCP_NODELAY、5ms 读超时、BLE 7.5ms 连接间隔、鼠标 8ms 合并
- **安全**：链路 TLS；设备 token + 中继 device/client 密钥；中继只转发不解析 KVM 帧

## 架构

```
蓝牙键鼠 ──BLE──> ESP32-S3 ──WiFi/UDP(局域网)──> PC Agent ──SendInput──> 本机
                     │
                     └──TLS(广域网)──> Relay Server <──TLS── PC Agent
```

| 组件 | 说明 |
|------|------|
| `firmware/` | ESP32-S3 固件（ESP-IDF 5.5） |
| `host/` | PC 端：GUI、Agent、协议、注入、发现 |
| `server/` | 自托管 TLS 中继（Python asyncio，无第三方依赖） |
| `docs/` | 协议、硬件、部署、排障 |

## 目录

```
firmware/            ESP-IDF 工程（main/ 为组件源码）
host/                PC 端 Python 代码
  kvm_gui.py         Tkinter 图形界面
  kvm_agent.py       命令行注入端（也用于 GUI 内部）
  kvmproto.py        KVM-Wire v1 协议
  discovery.py       局域网 UDP 发现
  engine.py          按键状态机 / 重复 / 看门狗
  hidmap.py          HID usage -> Windows VK
  wininject.py       SendInput 封装
  wan_relay.py       中继客户端
server/              relay_server.py + 部署模板
docs/                文档
packaging/           PyInstaller 打包与开始菜单安装脚本
```

## 硬件

- ESP32-S3-WROOM-1U-N8R8（8MB Flash / 8MB PSRAM，外置天线）
- USB 供电 + 串口（默认 `COM3`，115200）
- 板载 LED：GPIO2（低电平点亮）；BOOT：GPIO0

详见 [docs/hardware.md](docs/hardware.md)。

## 快速开始

### 1. 编译固件

需要 ESP-IDF **5.5.x**：

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

中继 CA 证书：仓库只带示例 `main/relay_ca.pem.example`。使用广域网前，把你的
CA 证书放到 `main/relay_ca.pem`（构建时会自动嵌入）。

### 2. 配网 + 连键鼠

1. 设备上电，电脑/手机连接开放热点 `KVM-Setup-XXXX`
2. 打开 <http://192.168.4.1/>
3. 「WiFi」填家里/办公室的 2.4G WiFi，保存
4. 「蓝牙键鼠」扫描 → 连接键盘和鼠标
5. 需要广域网时，在「广域网中继」里填中继地址、端口、设备密钥并启用

> AP 默认**无密码**，方便首次配网；可在 Kconfig 里改 `KVM_AP_PASSWORD`。

### 3. PC 端

依赖：Windows 10/11、Python 3.10+。

```powershell
cd host
python kvm_agent.py --scan          # 自动发现并选择设备
# 或者直接指定
python kvm_agent.py --peer 192.168.4.1:8766 --token 0123456789abcdef
```

图形界面：

```powershell
python host\kvm_gui.py
```

打包成单文件 exe 并注册到开始菜单：

```powershell
powershell -ExecutionPolicy Bypass -File packaging\build_exe.ps1
powershell -ExecutionPolicy Bypass -File packaging\install.ps1
```

### 4. 广域网

服务器部署见 [docs/DEPLOY.md](docs/DEPLOY.md)。

设备端启用中继后，PC 端：

```powershell
python host\kvm_agent.py --relay relay.example.com:8790 --relay-key <CLIENT_KEY>
```

## 延迟

实测（深圳 → 广州中继 → 深圳）：

| 场景 | RTT |
|------|-----|
| 局域网直连 | 3~15ms |
| 广域网空闲 | 30~50ms |
| 广域网鼠标持续移动 | 60~125ms |

已做的优化：三端 `TCP_NODELAY`、WAN 读超时 5ms / 轮询 4ms、BLE 连接间隔固定
7.5ms、鼠标 8ms 合并、WiFi 关闭省电。

## 安全模型

- 局域网：UDP 明文 + 16 位十六进制共享 token（**不要暴露 8766 到公网**）
- 广域网：TLS（自签 CA）+ 设备密钥/客户端密钥；中继只按 device_id 转发
- 建议：token/密钥随机生成；中继使用独立系统用户与目录；定期轮换密钥；
  不要把 `config.json`、CA 私钥、设备密钥提交到仓库

## 开发与测试

```bash
cd host
python -m unittest discover -s tests -v
python simulate.py --serve          # 无硬件时模拟设备
```

## 已知限制

- PC 注入仅支持 Windows（`SendInput`）
- BLE 5.0 扩展连接在部分 S3 控制器上被拒绝，固件强制 BLE 4.2
- BLE 键鼠重启后地址可能变化（RPA），需重新扫描连接
- 广域网目前经中继转发，延迟受中继位置影响；P2P 打洞是后续方向

## License

MIT，见 [LICENSE](LICENSE)。

