"""局域网设备自动发现。

PC 向局域网广播 DISCOVER，ESP32 收到后单播回一个 HELLO，
里面带设备名、MAC、固件版本和 token，用于界面展示与一键连接。
"""

from __future__ import annotations

import socket
import threading
import time
from dataclasses import dataclass, field

import kvmproto


@dataclass
class DiscoveredDevice:
    addr: str
    port: int
    name: str
    mac: str
    fw: str
    caps: int
    token: int
    proto: int
    last_seen: float = field(default_factory=time.monotonic)

    @property
    def key(self) -> str:
        return self.mac or f"{self.addr}:{self.port}"

    @property
    def token_hex(self) -> str:
        return f"{self.token:016x}"

    @property
    def is_hid_capable(self) -> bool:
        return bool(self.caps & 0x07)

    def describe(self) -> str:
        return f"{self.name or 'ESP32-S3 KVM'} @ {self.addr}:{self.port}"


class LanDiscovery:
    """可重复调用 scan() 的发现器，结果按 MAC 去重。"""

    def __init__(self, device_port: int = kvmproto.DEFAULT_DEVICE_PORT) -> None:
        self.device_port = device_port
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("", 0))
        self.sock.settimeout(0.2)
        self._lock = threading.Lock()
        self._devices: dict[str, DiscoveredDevice] = {}
        self._seq = 0
        self._closed = False

    # ------------------------------------------------------------------
    def broadcast_targets(self) -> list[str]:
        targets = {"255.255.255.255", "127.0.0.1"}
        try:
            for info in socket.getaddrinfo(socket.gethostname(), None, socket.AF_INET):
                ip = info[4][0]
                if ip.startswith("127.") or ip.startswith("169.254."):
                    continue
                parts = ip.split(".")
                if len(parts) == 4:
                    # 家庭/办公网络基本是 /24，直接算定向广播
                    targets.add(".".join(parts[:3] + ["255"]))
        except OSError:
            pass
        return sorted(targets)

    def _send_discover(self) -> None:
        self._seq = (self._seq + 1) & 0xFFFF
        payload = b""
        frame = kvmproto.encode(kvmproto.MSG_DISCOVER, self._seq, 0, payload, dev_ms=0)
        for target in self.broadcast_targets():
            try:
                self.sock.sendto(frame, (target, self.device_port))
            except OSError:
                pass

    def _record(self, data: bytes, addr: tuple[str, int]) -> None:
        try:
            frame = kvmproto.decode(data)
        except kvmproto.ProtocolError:
            return
        if frame.msg_type != kvmproto.MSG_HELLO:
            return
        try:
            hello = kvmproto.decode_hello(frame.payload)
        except kvmproto.ProtocolError:
            return
        mac = hello.mac.hex(":")
        device = DiscoveredDevice(
            addr=addr[0],
            port=addr[1],
            name=hello.name,
            mac=mac,
            fw=f"{hello.fw_major}.{hello.fw_minor}",
            caps=hello.caps,
            token=frame.token,
            proto=hello.proto_major,
        )
        with self._lock:
            self._devices[device.key] = device

    # ------------------------------------------------------------------
    def scan(self, seconds: float = 3.0) -> list[DiscoveredDevice]:
        """阻塞扫描一段时间，返回当前发现的设备（按最近出现排序）。"""
        deadline = time.monotonic() + max(0.5, seconds)
        next_send = 0.0
        while time.monotonic() < deadline and not self._closed:
            now = time.monotonic()
            if now >= next_send:
                self._send_discover()
                next_send = now + 0.4
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            self._record(data, addr)
        return self.devices()

    def devices(self) -> list[DiscoveredDevice]:
        with self._lock:
            return sorted(self._devices.values(), key=lambda d: d.last_seen, reverse=True)

    def clear(self) -> None:
        with self._lock:
            self._devices.clear()

    def close(self) -> None:
        self._closed = True
        try:
            self.sock.close()
        except OSError:
            pass
