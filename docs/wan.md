# 广域网（WAN）方案

> 状态：**已实现**。设备与 PC 都主动连接 TLS 中继，按 `device_id` 配对后由中继
> 转发 KVM 帧。部署步骤见 [DEPLOY.md](DEPLOY.md)。

局域网只是第一阶段验证。最终目标是「设备放家里、人在公司/外地也能用键鼠」。本文件说明架构、协议和取舍。

## 1. 核心问题

| 问题 | 局域网 | 广域网 |
|------|--------|--------|
| 寻址 | UDP 广播 + IP:8766 | 双方都没有公网 IP，需要中继 |
| 发现 | DISCOVER 广播 | 中继返回在线设备列表 |
| 安全 | 共享 token | TLS + 设备密钥/客户端密钥 |

## 2. 架构

```
ESP32 ──TLS──> ┌──────────────┐ <──TLS── PC
               │ Relay Server │
               └──────────────┘
```

- 设备启动后连中继，发送 `HELLO_DEVICE`（device_key + device_id）
- PC 连中继，发送 `HELLO_CLIENT`（client_key）
- PC 发 `CONNECT` 指定 device_id，中继给双方发 `PEER_READY`
- 之后双方的 `DATA` 帧原样转发（内容是完整的 KVM-Wire 帧）

## 3. 中继帧格式

```
[u32 长度][u8 类型][payload]
```

| 类型 | 名称 | 方向 |
|------|------|------|
| 1 | HELLO_DEVICE | 设备 → 中继 |
| 2 | HELLO_CLIENT | PC → 中继 |
| 3 | HELLO_OK | 中继 → 双方 |
| 4 | DEVICE_LIST | 中继 → PC |
| 5 | CONNECT | PC → 中继 |
| 6 | PEER_READY | 中继 → 双方 |
| 7 | DATA | 双向（KVM 帧） |
| 8/9 | PING/PONG | 双向 |
| 10 | ERROR | 中继 → 客户端 |
| 11 | PEER_GONE | 中继 → 对端 |
| 12 | BYE | 客户端 → 中继 |

KVM 帧本身的 0x10~0x1F 保留给未来的 WAN 扩展。

## 4. 安全

- 中继使用自签 CA + 服务器证书，客户端校验 CA
- `device_key` 只存设备 NVS 与服务器配置；`client_key` 只给 PC 端
- 中继按 device_id 转发，不解析键鼠内容
- 生产环境建议：每台设备独立密钥、定期轮换、限制来源 IP

## 5. 延迟与优化

| 优化 | 说明 |
|------|------|
| TCP_NODELAY | 三端关闭 Nagle，避免 40ms 级延迟 |
| 读超时 5ms / 轮询 4ms | 设备侧发送不再等大超时 |
| BLE 连接间隔 7.5ms | 降低 HID 上报周期 |
| 鼠标 8ms 合并 | 降低帧率与带宽 |

实测（深圳 → 广州中继）：空闲 RTT 30~50ms，鼠标持续移动 60~125ms。

## 6. 后续方向

- **P2P 打洞**：STUN + UDP 直连，绕开中继，目标 10~30ms
- **UDP/QUIC 中继**：避免 TCP 重传造成的尖峰
- **多中继节点**：按地理位置就近接入
- **端到端加密**：中继不可见键鼠内容（当前 TLS 只到中继）
