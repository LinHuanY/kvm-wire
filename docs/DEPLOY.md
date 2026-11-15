# 广域网中继部署指南

中继是一个**无第三方依赖**的 Python asyncio 服务：设备与 PC 都主动向它建立 TLS
连接，注册/配对后由中继转发 KVM 帧。适合部署在一台有公网 IP 的 Linux 服务器上。

> 中继只做转发，不解析键鼠内容；但密钥、证书仍需妥善保管。

## 1. 环境要求

- Ubuntu 22.04 / Debian 12（其他 Linux 亦可）
- Python **3.10+**（系统自带即可，不需要 pip 安装任何包）
- 一个公网 TCP 端口（示例用 `8790`）
- 建议 1 核 512MB 以上；实际内存占用约 15MB

## 2. 创建独立用户与目录

```bash
sudo useradd --system --no-create-home --shell /usr/sbin/nologin ipkvm
sudo mkdir -p /opt/ipkvm-relay/tls
sudo chown -R ipkvm:ipkvm /opt/ipkvm-relay
sudo chmod 750 /opt/ipkvm-relay
```

## 3. 生成 TLS 证书

把仓库里的 `server/gen_certs.sh` 上传到服务器，然后：

```bash
cd /opt/ipkvm-relay
sudo bash gen_certs.sh relay.example.com tls
```

- 参数 1：设备/PC 连接时使用的主机名或公网 IP（会写进证书 SAN）
- 参数 2：证书输出目录（默认 `tls`）

脚本会生成：

```
tls/ca.crt        客户端需要信任的 CA（公开）
tls/ca.key        CA 私钥（不要外泄）
tls/server.crt    服务器证书
tls/server.key    服务器私钥
```

> 客户端（ESP32 固件 / PC 端）只需要 `ca.crt`。

## 4. 生成设备密钥与客户端密钥

```bash
python3 -c "import secrets;print(secrets.token_hex(24))"   # device_key
python3 -c "import secrets;print(secrets.token_hex(24))"   # client_key
```

写入配置：

```bash
sudo tee /opt/ipkvm-relay/config.json >/dev/null <<'JSON'
{
  "listen": "0.0.0.0",
  "port": 8790,
  "cert": "tls/server.crt",
  "key": "tls/server.key",
  "device_key": "把 device_key 填在这里",
  "client_key": "把 client_key 填在这里"
}
JSON
```

> `device_key` 填到 ESP32 网页的「广域网中继 → 设备密钥」；
> `client_key` 给 PC 端 `--relay-key` 使用。

## 5. 安装 systemd 服务

```bash
sudo cp server/ipkvm-relay.service.example /etc/systemd/system/ipkvm-relay.service
sudo systemctl daemon-reload
sudo systemctl enable --now ipkvm-relay
sudo systemctl status ipkvm-relay --no-pager
```

日志：

```bash
journalctl -u ipkvm-relay -f
```

看到下面这行即成功：

```
INFO IPKVM relay listening on ('0.0.0.0', 8790) (TLS)
```

## 6. 放行端口

云服务器需要在**安全组/防火墙**放行 TCP 8790（这是云平台控制台，不是 Linux 命令）。
本机防火墙（如启用 ufw）：

```bash
sudo ufw allow 8790/tcp
```

验证：

```bash
# 从你自己的电脑
nc -vz relay.example.com 8790
```

## 7. 客户端配置

### ESP32

1. 把 `tls/ca.crt` 内容保存为固件里的 `firmware/main/relay_ca.pem`
2. 编译烧录
3. 设备网页 →「广域网中继」：
   - 中继地址：`relay.example.com`
   - 端口：`8790`
   - 设备密钥：上面生成的 `device_key`
   - 勾选启用

### PC

```powershell
python host\kvm_agent.py --relay relay.example.com:8790 --relay-key <client_key>
```

PC 端需要信任中继的 CA。默认读取仓库里的 `server/ca.crt`；也可用 `--relay-ca`
指定路径。

设备网页会显示一个 32 位**设备配对码**（首次启动自动生成）。PC 端连接时填写同一个码：

```powershell
python host\kvm_agent.py --relay relay.example.com:8790 --relay-key <client_key> --pair-code <配对码>
```

配对码用于端到端 AES-256-GCM 加密：中继只转发密文，配对码错误时 PC 收不到可解密的数据。

## 8. 验证

服务器上：

```bash
journalctl -u ipkvm-relay -n 20 --no-pager
```

正常会看到：

```
新连接 x.x.x.x:xxxxx
设备上线 <device_id> (esp32s3-kvm)
客户端上线
配对成功 device=<device_id>
```

PC 端看到 `[link] 已通过中继配对设备 ...` 即通。

## 9. 升级与卸载

```bash
# 升级（覆盖程序后重启）
sudo systemctl restart ipkvm-relay

# 卸载
sudo systemctl disable --now ipkvm-relay
sudo rm /etc/systemd/system/ipkvm-relay.service
sudo rm -rf /opt/ipkvm-relay
sudo userdel ipkvm
```

## 10. 安全建议

- 中继使用独立系统用户，目录权限 `750`
- `config.json`、`tls/*.key` 权限 `640`，属主 `ipkvm`
- 定期轮换 `device_key` / `client_key`：改配置后重启，并同步改设备/PC
- 不要把 `config.json`、CA 私钥提交到 Git
- 服务器 SSH 建议只允许密钥登录，禁用 root 密码登录
