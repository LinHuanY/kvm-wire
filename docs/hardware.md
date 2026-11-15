# 硬件要点（ESP32S3-Cam V1.0）

> 依据原理图 `4.SCH_S3-Cam V1.0.pdf`（幻尔科技，2024-07）。本工程只用到 WiFi/BLE 和两个板载 IO，不驱动摄像头。

## 模组

| 项目 | 参数 |
|------|------|
| 模组 | **ESP32-S3-WROOM-1U-N8R8** |
| Flash | 8 MB（分区表按 8MB 配置，app 3MB） |
| PSRAM | 8 MB Octal（本工程**未启用**，不需要） |
| 天线 | **1U = 外置天线**，板上是 U.FL/IPEX 座 |

> ⚠️ WROOM-1U 没有板载 PCB 天线，**必须接 2.4GHz 外置天线**，否则蓝牙和 WiFi 距离会非常短（隔一两米就断）。这是最容易忽略的一点。

## 板载 IO

| 功能 | GPIO | 说明 |
|------|------|------|
| 蓝色 LED | **GPIO2** | `VDD33 → 1K → LED → GPIO2`，**低电平点亮**（`KVM_LED_ACTIVE_LOW=y`） |
| BOOT 按键 | **GPIO0** | 10K 上拉到 3.3V，按下接地；固件里长按 5 秒恢复出厂 |
| 串口 TX/RX | GPIO43 / GPIO44 | `TXD0/RXD0`，接 USB-TTL 烧录/看日志 |
| 摄像头 | GPIO4~18 等 | 本工程不使用，FPC 可空着 |

## 供电与射频

- 5V 输入，板载 AMS1117-3.3。WiFi 发射瞬间电流可达 300~400mA，**USB 口/电源要能稳定供 500mA 以上**，否则会出现反复重启、蓝牙掉线。
- 蓝牙与 WiFi 共用 2.4GHz 射频，固件已打开软件共存（`CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y`）。
- 尽量让 ESP32 与键鼠之间无金属遮挡；USB 3.0 设备/硬盘盒会干扰 2.4GHz。

## 烧录

原理图上 `TXD0/RXD0` 引到排针，`IO0` 接 BOOT 键。两种方式：

1. **USB-TTL（推荐，最稳）**：TX↔RX 交叉，GND 共地，3.3V 供电；烧录时按住 BOOT 再上电/复位。
2. **板载 USB**：若你的板子把 GPIO19/20（D-/D+）引到了 USB 座，可直接用原生 USB 烧录（`idf.py -p COMx flash`）。

烧录后看日志：

```powershell
idf.py -p COM5 monitor
```

正常启动会看到：

```
I (xxx) kvm: ==== ESP32-S3 KVM Bridge (esp32s3) ====
I (xxx) kvm_wifi: AP 已启动: SSID=KVM-Setup-XXXX  http://192.168.4.1/
I (xxx) kvm_ble: BLE HID Host 已初始化
I (xxx) kvm_http: 配置页: http://192.168.4.1/  (AP: KVM-Setup-XXXX)
```

## 与摄像头的冲突

摄像头 FPC 占用了 GPIO4~18、GPIO39~42 等。本工程只碰 GPIO0/GPIO2 和 WiFi/BLE 射频，**不会和摄像头冲突**。以后若要在同一块板上加视频采集，再单独分配 I2S/LCD 引脚即可。
