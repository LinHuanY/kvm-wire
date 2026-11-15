#!/usr/bin/env python3
"""KVM-Wire PC 注入端（Windows）。

用法：
    python kvm_agent.py --token 0123456789abcdef
    python kvm_agent.py --dry-run          # 只打印事件，不注入
    python kvm_agent.py --config config.json

局域网内 ESP32-S3 通过 UDP 把键鼠事件发到这里，再由 SendInput 注入。
"""

from __future__ import annotations

import argparse
import json
import socket
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

import engine
import kvmproto

DEFAULT_PORT = 8765


@dataclass
class AgentConfig:
    bind: str = "0.0.0.0"
    port: int = DEFAULT_PORT
    token: int | None = None          # None = 不校验（仅调试）
    peer_ip: str | None = None        # 确认连接的设备地址
    peer_port: int | None = None
    relay_host: str | None = None     # 广域网中继地址
    relay_port: int = 8790
    relay_ca: str | None = None
    relay_key: str | None = None
    relay_device: str | None = None
    mouse_mode: str = "track"         # track | relative
    mouse_scale: float = 1.0
    wheel_scale: int = 1
    repeat: bool = True
    repeat_delay: float = 0.50
    repeat_interval: float = 0.033
    watchdog: float = 3.0
    verbose: bool = False
    dry_run: bool = False
    stats_interval: float = 5.0
    ping_interval: float = 1.0


def local_ipv4_addresses() -> list[str]:
    addresses = set()
    try:
        infos = socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET)
        addresses.update(info[4][0] for info in infos)
    except OSError:
        pass
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("8.8.8.8", 80))
        addresses.add(probe.getsockname()[0])
        probe.close()
    except OSError:
        pass
    return sorted(a for a in addresses if not a.startswith("127."))


class KvmAgent:
    def __init__(self, cfg: AgentConfig) -> None:
        self.cfg = cfg
        self.sink = engine.PrintSink() if cfg.dry_run else self._make_injector()
        self.engine = engine.InputEngine(
            self.sink,
            engine.EngineConfig(
                repeat=cfg.repeat,
                repeat_delay=cfg.repeat_delay,
                repeat_interval=cfg.repeat_interval,
                mouse_scale=cfg.mouse_scale,
                wheel_scale=cfg.wheel_scale,
                watchdog_timeout=cfg.watchdog,
                verbose=cfg.verbose,
            ),
        )
        self._sock: socket.socket | None = None
        self.relay = None
        self._peer: tuple[str, int] | None = None
        self._stop = threading.Event()
        self._rx_thread: threading.Thread | None = None
        self._lock = threading.Lock()
        self._last_ping = 0.0
        self._last_stats = time.monotonic()
        self._rtt_ms: float | None = None
        self._resynced = False
        self._seq = 0
        self._rx_count = 0
        self._decided_token: int | None = cfg.token

    # ------------------------------------------------------------------
    def _make_injector(self):
        if sys.platform != "win32":
            raise SystemExit("当前注入端只支持 Windows；在其它系统请用 --dry-run 查看事件。")
        from wininject import WindowsInjector

        return WindowsInjector(mouse_mode=self.cfg.mouse_mode)

    def _next_seq(self) -> int:
        self._seq = (self._seq + 1) & 0xFFFF
        return self._seq

    def _send(self, msg_type: int, payload: bytes = b"") -> None:
        with self._lock:
            peer = self._peer
            sock = self._sock
            token = self._decided_token
            relay = self.relay
        frame_token = token if token is not None else 0
        frame = kvmproto.encode(msg_type, self._next_seq(), frame_token, payload, dev_ms=0)
        if relay is not None:
            try:
                relay.send(frame)
            except Exception:  # noqa: BLE001 - 中继断开时静默
                pass
            return
        if peer is None or sock is None or token is None:
            return
        try:
            sock.sendto(frame, peer)
        except OSError:
            pass

    # ------------------------------------------------------------------
    def start(self) -> None:
        if self.cfg.relay_host:
            self._start_relay()
            return

        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        sock.bind((self.cfg.bind, self.cfg.port))
        sock.settimeout(0.2)
        self._sock = sock

        print("=" * 62)
        print("KVM-Wire PC 注入端")
        print(f"  监听        : {self.cfg.bind}:{self.cfg.port}/udp")
        print(f"  鼠标模式    : {self.cfg.mouse_mode}  (scale={self.cfg.mouse_scale})")
        print(f"  按键重复    : {'开' if self.cfg.repeat else '关'}  "
              f"({self.cfg.repeat_delay:.2f}s 后每 {self.cfg.repeat_interval * 1000:.0f}ms)")
        print(f"  链路看门狗  : {self.cfg.watchdog:.1f}s")
        if self.cfg.peer_ip:
            peer_port = self.cfg.peer_port or kvmproto.DEFAULT_DEVICE_PORT
            print(f"  目标设备    : {self.cfg.peer_ip}:{peer_port}")
        if self.cfg.token is None:
            print("  token       : 未设置（接受任意 token；首次运行建议记下设备 token）")
        else:
            print(f"  token       : {self.cfg.token:016x}")
        if self.cfg.dry_run:
            print("  模式        : DRY-RUN（只打印，不注入）")
        ips = local_ipv4_addresses()
        if ips:
            print("  本机 IPv4   : " + ", ".join(ips))
            print(f"  固件里请填  : KVM_HOST_IP=\"{ips[0]}\"  KVM_HOST_PORT={self.cfg.port}")
        print("  Ctrl+C 退出（退出前会释放所有按键）")
        print("=" * 62)

        if self.cfg.peer_ip:
            peer = (self.cfg.peer_ip, self.cfg.peer_port or kvmproto.DEFAULT_DEVICE_PORT)
            with self._lock:
                self._peer = peer
            if self._decided_token is None:
                # 不知道 token 时，先向设备发一次 DISCOVER 让它回 HELLO
                try:
                    sock.sendto(kvmproto.encode(kvmproto.MSG_DISCOVER, 0, 0, b""), peer)
                except OSError:
                    pass

        self._rx_thread = threading.Thread(target=self._rx_loop, name="kvm-rx", daemon=True)
        self._rx_thread.start()

    def _rx_loop(self) -> None:
        assert self._sock is not None
        while not self._stop.is_set():
            try:
                data, addr = self._sock.recvfrom(4096)
            except socket.timeout:
                continue
            except OSError:
                break
            self._rx_count += 1
            self._handle_datagram(data, addr)

    # ------------------------------------------------------------------
    # 广域网中继
    # ------------------------------------------------------------------
    def _start_relay(self) -> None:
        from wan_relay import RelayClient

        print("=" * 62)
        print("KVM-Wire PC 注入端（广域网中继）")
        print(f"  中继        : {self.cfg.relay_host}:{self.cfg.relay_port} (TLS)")
        print(f"  鼠标模式    : {self.cfg.mouse_mode}  (scale={self.cfg.mouse_scale})")
        print(f"  链路看门狗  : {self.cfg.watchdog:.1f}s")
        if self.cfg.dry_run:
            print("  模式        : DRY-RUN（只打印，不注入）")
        print("=" * 62)

        self.relay = RelayClient(self.cfg.relay_host, self.cfg.relay_port,
                                 self.cfg.relay_ca, self.cfg.relay_key)
        try:
            self.relay.connect()
        except Exception as exc:  # noqa: BLE001
            raise SystemExit(f"[fail] 连接中继失败: {exc}") from exc

        devices = self.relay.devices
        if not devices:
            raise SystemExit("[fail] 中继上没有在线设备（确认 ESP32 已连到中继）")
        target = self.cfg.relay_device or devices[0]["device_id"]
        print(f"  在线设备    : {devices}")
        if not self.relay.open_device(target):
            raise SystemExit(f"[fail] 与设备 {target} 配对失败: {self.relay.last_error}")
        print(f"[link] 已通过中继配对设备 {target}")

        if self._decided_token is None:
            # 不知道设备 token 时，先发 DISCOVER 让设备回 HELLO
            self._send(kvmproto.MSG_DISCOVER)

        self._rx_thread = threading.Thread(target=self._relay_rx_loop, name="kvm-relay-rx", daemon=True)
        self._rx_thread.start()

    def _relay_rx_loop(self) -> None:
        while not self._stop.is_set():
            try:
                data = self.relay.recv(0.2)
            except Exception:  # noqa: BLE001
                break
            if data:
                self._rx_count += 1
                self._handle_datagram(data, ("relay", self.cfg.relay_port))

    def _handle_datagram(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            frame = kvmproto.decode(data)
        except kvmproto.ProtocolError as exc:
            self.engine.stats.bad += 1
            if self.engine.stats.bad <= 5 or self.cfg.verbose:
                print(f"[warn] 丢弃非法帧({len(data)}B): {exc}")
            return

        if self._decided_token is None:
            self._decided_token = frame.token
            print(f"[link] 已锁定设备 token = {frame.token:016x}（请填进固件配置）")
        elif frame.token != self._decided_token:
            self.engine.stats.bad += 1
            if self.engine.stats.bad <= 5:
                print(f"[warn] token 不匹配，来自 {addr[0]}:{addr[1]}")
            return

        with self._lock:
            self._peer = addr

        if frame.msg_type == kvmproto.MSG_PONG:
            host_ms, _dev_ms = kvmproto.decode_pong(frame.payload)
            now_ms = int(time.monotonic() * 1000)
            self._rtt_ms = max(0.0, float((now_ms - host_ms) & 0xFFFFFFFF))

        self.engine.handle_frame(frame)

        if not self._resynced and frame.msg_type in (kvmproto.MSG_HELLO, kvmproto.MSG_KEY, kvmproto.MSG_MOUSE):
            self._resynced = True
            self._send(kvmproto.MSG_CMD, kvmproto.cmd_payload(kvmproto.CMD_RESYNC))

    # ------------------------------------------------------------------
    def run(self) -> None:
        self.start()
        try:
            while not self._stop.is_set():
                now = time.monotonic()
                self.engine.tick(now)
                if hasattr(self.sink, "tick"):
                    self.sink.tick(now)
                if now - self._last_ping >= self.cfg.ping_interval:
                    self._last_ping = now
                    self._send(kvmproto.MSG_PING, kvmproto.ping_payload(int(now * 1000)))
                if self.cfg.stats_interval > 0 and now - self._last_stats >= self.cfg.stats_interval:
                    self._last_stats = now
                    self._print_stats(now)
                time.sleep(0.01)
        except KeyboardInterrupt:
            print("\n[exit] 收到 Ctrl+C")
        finally:
            self.stop()

    def _print_stats(self, now: float) -> None:
        st = self.engine.stats
        if not st.frames:
            return
        rtt = f"{self._rtt_ms:.1f}ms" if self._rtt_ms is not None else "-"
        print(
            f"[stats] 帧={st.frames} 键={st.keys} 鼠标={st.mouse} 丢包={st.lost} "
            f"速率={st.pps(now):.1f}/s RTT≈{rtt}"
        )

    def request_release(self) -> None:
        """释放所有按键，并通知设备重置状态。"""
        self.engine.release_all("manual")
        self._send(kvmproto.MSG_CMD, kvmproto.cmd_payload(kvmproto.CMD_RELEASE_ALL))

    @property
    def rtt_ms(self) -> float | None:
        return self._rtt_ms

    def stop(self) -> None:
        self._stop.set()
        try:
            self.engine.release_all("agent exit")
        except Exception as exc:  # noqa: BLE001 - 退出路径尽量不抛
            print(f"[warn] 释放按键失败: {exc}")
        if self._rx_thread is not None:
            self._rx_thread.join(timeout=1.0)
        if self.relay is not None:
            try:
                self.relay.close()
            except Exception:  # noqa: BLE001
                pass
        if self._sock is not None:
            self._sock.close()
        print("[exit] 已停止")


def load_config(path: str) -> AgentConfig:
    with open(path, "r", encoding="utf-8") as fh:
        raw = json.load(fh)
    cfg = AgentConfig()
    for key, value in raw.items():
        if key == "token":
            cfg.token = kvmproto.parse_token(str(value)) if value not in (None, "", 0) else None
        elif hasattr(cfg, key):
            setattr(cfg, key, value)
    return cfg


def parse_peer(text: str, default_port: int) -> tuple[str, int]:
    if ":" in text:
        host, port = text.rsplit(":", 1)
        return host, int(port)
    return text, default_port


def select_device(device_port: int):
    """扫描局域网并让用户选择要连接的设备。"""
    from discovery import LanDiscovery

    disc = LanDiscovery(device_port)
    print("正在扫描局域网内的 ESP32-S3 KVM 设备（3 秒）...")
    devices = disc.scan(3.0)
    disc.close()
    if not devices:
        print("没有发现设备。请检查：")
        print("  1. 设备已上电，且和电脑连在同一个局域网")
        print("  2. 设备网页里已连上 WiFi")
        print("  3. Windows 防火墙放行 UDP（设备端口 8766）")
        return None
    print(f"发现 {len(devices)} 台设备：")
    for i, d in enumerate(devices):
        print(f"  [{i}] {d.name or 'ESP32-S3 KVM'}  {d.addr}:{d.port}  MAC={d.mac}  fw={d.fw}  token={d.token_hex}")
    while True:
        try:
            raw = input("选择要连接的设备编号（回车退出）：").strip()
        except (EOFError, KeyboardInterrupt):
            return None
        if not raw:
            return None
        try:
            idx = int(raw)
        except ValueError:
            print("请输入数字编号")
            continue
        if 0 <= idx < len(devices):
            return devices[idx]
        print("编号超出范围")


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="KVM-Wire PC 注入端")
    p.add_argument("--config", help="JSON 配置文件")
    p.add_argument("--bind", default=None, help="监听地址，默认 0.0.0.0")
    p.add_argument("--port", type=int, default=None, help=f"UDP 端口，默认 {DEFAULT_PORT}")
    p.add_argument("--scan", action="store_true", help="自动发现局域网设备并选择连接")
    p.add_argument("--peer", help="直接连接设备，格式 IP[:端口]，端口默认 8766")
    p.add_argument("--relay", help="广域网中继 host[:端口]，例如 relay.example.com:8790")
    p.add_argument("--relay-ca", help="中继 CA 证书路径（默认 ../server/ca.crt）")
    p.add_argument("--relay-key", help="中继客户端密钥")
    p.add_argument("--relay-device", help="中继上的设备 ID（默认选第一个在线的）")
    p.add_argument("--device-port", type=int, default=8766, help="设备监听端口，默认 8766")
    p.add_argument("--token", default=None, help="16 位十六进制共享 token")
    p.add_argument("--mouse-mode", choices=["track", "relative"], default=None,
                   help="track=按光标位置定位(推荐)，relative=相对位移")
    p.add_argument("--scale", type=float, default=None, help="鼠标灵敏度倍率")
    p.add_argument("--wheel-scale", type=int, default=None, help="滚轮倍率（默认 1）")
    p.add_argument("--no-repeat", action="store_true", help="关闭按键长按重复")
    p.add_argument("--repeat-delay", type=float, default=None, help="长按多少秒后开始重复")
    p.add_argument("--timeout", type=float, default=None, help="链路看门狗秒数")
    p.add_argument("--dry-run", action="store_true", help="只打印事件，不注入")
    p.add_argument("--verbose", action="store_true", help="打印每个事件")
    p.add_argument("--stats-interval", type=float, default=None, help="统计打印间隔，0=关闭")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    cfg = load_config(args.config) if args.config else AgentConfig()

    if args.bind is not None:
        cfg.bind = args.bind
    if args.port is not None:
        cfg.port = args.port
    if args.token is not None:
        cfg.token = kvmproto.parse_token(args.token)
    if args.mouse_mode is not None:
        cfg.mouse_mode = args.mouse_mode
    if args.scale is not None:
        cfg.mouse_scale = args.scale
    if args.wheel_scale is not None:
        cfg.wheel_scale = args.wheel_scale
    if args.no_repeat:
        cfg.repeat = False
    if args.repeat_delay is not None:
        cfg.repeat_delay = args.repeat_delay
    if args.timeout is not None:
        cfg.watchdog = args.timeout
    if args.dry_run:
        cfg.dry_run = True
    if args.verbose:
        cfg.verbose = True
    if args.stats_interval is not None:
        cfg.stats_interval = args.stats_interval

    if args.relay:
        host, port_text = args.relay, ""
        if ":" in args.relay:
            host, port_text = args.relay.rsplit(":", 1)
        cfg.relay_host = host
        cfg.relay_port = int(port_text) if port_text else 8790
        cfg.relay_ca = args.relay_ca or str(Path(__file__).resolve().parent.parent / "server" / "ca.crt")
        if not args.relay_key:
            print("--relay 需要同时提供 --relay-key（中继客户端密钥）")
            return 2
        cfg.relay_key = args.relay_key
        cfg.relay_device = args.relay_device

    if args.peer:
        host, port = parse_peer(args.peer, args.device_port)
        cfg.peer_ip, cfg.peer_port = host, port
    elif args.scan:
        device = select_device(args.device_port)
        if device is None:
            return 1
        cfg.peer_ip, cfg.peer_port = device.addr, device.port
        if args.token is None:
            cfg.token = device.token
            print(f"[link] 已自动使用设备 token: {device.token_hex}")

    KvmAgent(cfg).run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
