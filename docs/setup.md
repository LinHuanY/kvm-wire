# 搭建与联调（Windows）

## 0. 环境

| 组件 | 版本 / 位置 |
|------|-------------|
| ESP-IDF | **v5.5.5**，`esp-idf`（5.5.x） |
| Python（PC 注入端） | 3.9+，只用到标准库 |
| 目标芯片 | ESP32-S3（8MB Flash） |

> 本机还装过 v5.1 / v6.0.2，但它们的子模块不完整（缺 mbedtls、nimble、esp_wifi 预编译库），无法编译。**请统一使用 v5.5.5**。

## 1. 编译固件

```powershell
cd firmware
.\build.ps1 -Clean
```

首次会编译整个 IDF（本机实测约 40 分钟，之后增量编译只要几十秒）。成功后会输出：

```
Project build complete. To flash, run:
 idf.py flash
```

产物：`firmware\build\esp32s3-kvm.bin`（固件）、`build\bootloader\bootloader.bin`、`build\partition_table\partition-table.bin`。

## 2. 烧录

USB-TTL 接 `TXD0(GPIO43)`、`RXD0(GPIO44)`、GND；烧录时按住 BOOT 再上电。

```powershell
.\build.ps1 -Flash -Monitor -Port COM5
```

退出监视：`Ctrl+]`。

## 3. 首次配置

1. 手机/电脑连 `KVM-Setup-XXXX`（**默认无密码/开放热点**）
2. 浏览器打开 `http://192.168.4.1/`
3. **连接 WiFi**：扫描 → 选 2.4GHz 的 SSID → 输密码 → 保存
4. **PC 注入端**：填电脑 IP（留空则自动发现）、端口 8765、token
5. **蓝牙键鼠**：键鼠进配对模式 → 扫描 → 连接

## 4. 电脑端

### 4.1 图形控制台（推荐）

```powershell
cd host
python kvm_gui.py        # 或双击“启动控制台.bat”
```

启动后自动扫描局域网，设备会出现在列表里（名称 / IP:端口 / MAC / 固件 / token）。
选中设备 →「连接选中设备」→ 弹窗确认 → 开始注入。没点确认前不会注入任何输入。

窗口里还可以设置 token、鼠标模式、灵敏度、滚轮倍率、长按重复，并实时看收包/丢包/RTT。

### 4.2 命令行

```powershell
# 自动发现并选择
python kvm_agent.py --scan

# 已知 IP 直连
python kvm_agent.py --peer 192.168.1.50:8766

# 不知道 token 也行：agent 会向设备发 DISCOVER，从 HELLO 里自动学习
python kvm_agent.py --peer 192.168.1.50

# 只打印不注入
python kvm_agent.py --scan --dry-run --verbose
```

首次运行 Windows 会弹防火墙，允许「专用网络」。程序会打印：

```
监听        : 0.0.0.0:8765/udp
本机 IPv4   : 192.168.1.23
固件里请填  : KVM_HOST_IP="192.168.1.23"  KVM_HOST_PORT=8765
```

参数：

```text
--scan              广播发现设备并选择连接
--peer IP[:端口]     直接连接设备（端口默认 8766）
--device-port 8766  设备监听端口（自动发现用）
--dry-run           只打印事件，不注入（先这样验证链路）
--verbose           打印每个按键/鼠标事件
--mouse-mode track  绝对定位（默认，1:1 不飘）
--mouse-mode relative  相对位移（个别游戏/软件更兼容）
--scale 1.5         鼠标灵敏度
--no-repeat         关闭长按重复
--timeout 3.0       链路看门狗秒数
--config config.json 从配置文件读
--stats-interval 5  统计打印间隔，0 关闭
```

优雅退出：`Ctrl+C`，程序会自动释放所有按下的键和鼠标键。

## 5. 无硬件测试链路

```powershell
# 终端 A：注入端只看不点
python kvm_agent.py --dry-run --token 0123456789abcdef

# 终端 B：模拟器（监听 8766，会回应 DISCOVER，可被 GUI 发现）
python simulate.py --serve --token 0123456789abcdef
```

现在可以在 GUI 里点「刷新设备」，应该能看到 `simulator-esp32s3`，连接后状态区 RTT 会更新。
要看按键/鼠标事件，让模拟器跑演示序列：

```powershell
python simulate.py --token 0123456789abcdef --demo
```

终端 A 应依次打印 HELLO、按键、鼠标位移、滚轮、音量键等事件。
确认无误后，去掉 `--dry-run` 再跑一次，这次会真的操作电脑（先把记事本切到前台）。

## 6. 日志与调试

```powershell
idf.py -p COM5 monitor
# monitor 里可以实时改日志级别：
log_level kvm_ble 4        # 打印原始 HID 报告
log_level kvm_net 4
```

设备重启后恢复默认级别。

## 7. 常见配置改法

| 想改什么 | 怎么做 |
|----------|--------|
| 换热点名/密码 | `idf.py menuconfig` → KVM Bridge 配置 |
| 换默认 token | 同上，`KVM_DEFAULT_TOKEN` |
| 默认连家里 WiFi | `KVM_DEFAULT_STA_SSID/PASS`，烧录即连 |
| 不用状态灯/按键 | `KVM_STATUS_LED_GPIO=-1`、`KVM_BUTTON_GPIO=-1` |
| 配对失败的设备 | 关闭 `KVM_BLE_BOND` 后重试 |

改完 menuconfig 要重新 `idf.py build` 并烧录。
