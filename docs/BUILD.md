# 编译与打包

## 1. 固件（ESP32-S3）

环境：ESP-IDF **5.5.x**（v5.1 / v6.0 的组件不完整时可能编译失败）。

```bash
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

### 中继 CA

仓库自带示例 CA `main/relay_ca.pem.example`。首次构建时若 `main/relay_ca.pem`
不存在，CMake 会自动复制示例；使用真实中继前请替换为你的 CA：

```bash
cp /path/to/your/ca.crt firmware/main/relay_ca.pem
idf.py build
```

`relay_ca.pem` 已在 `.gitignore` 中，不会被提交。

### 主要 Kconfig

| 选项 | 默认 | 说明 |
|------|------|------|
| `KVM_DEFAULT_HOST_IP` | 空 | PC 固定 IP，空=广播发现 |
| `KVM_DEVICE_PORT` | 8766 | 设备 UDP 端口 |
| `KVM_DEFAULT_TOKEN` | `0123456789abcdef` | 局域网共享 token |
| `KVM_DEFAULT_RELAY_HOST` | 空 | 中继地址（也可在网页填） |
| `KVM_DEFAULT_RELAY_PORT` | 8790 | 中继端口 |
| `KVM_AP_SSID_PREFIX` | `KVM-Setup` | 热点前缀 |
| `KVM_AP_PASSWORD` | 空 | 空=开放热点 |
| `KVM_STATUS_LED_GPIO` | 2 | 状态灯 |

## 2. PC 端 exe

环境：Windows 10/11、Python 3.10+、`pip install pyinstaller pillow`。

```powershell
powershell -ExecutionPolicy Bypass -File packaging\build_exe.ps1
```

产物：`dist\KVM-Wire.exe`（单文件、无控制台、带图标）。

### 安装到开始菜单

```powershell
powershell -ExecutionPolicy Bypass -File packaging\install.ps1
```

会：

- 复制到 `%LOCALAPPDATA%\Programs\KVM-Wire`
- 在**开始菜单**创建 `KVM-Wire.lnk`（应用会出现在开始菜单/所有应用）
- 在桌面创建快捷方式

卸载：删除上面的目录和两个 `.lnk` 即可。

## 3. 运行测试

```powershell
cd host
python -m unittest discover -s tests -v
```

## 4. 发布

```powershell
gh release create v0.1.0 dist\KVM-Wire.exe --title "v0.1.0" --notes "first release"
```
