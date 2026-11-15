"""KVM-Wire v1 协议编解码（纯标准库）。

帧格式见 docs/protocol.md。
"""

from __future__ import annotations

import struct
from dataclasses import dataclass, field
from typing import Optional

MAGIC0 = 0x4B  # 'K'
MAGIC1 = 0x56  # 'V'
VERSION = 1

HEADER_SIZE = 21
MAX_PAYLOAD = 255

# 消息类型
MSG_HELLO = 0x01
MSG_KEY = 0x02
MSG_MOUSE = 0x03
MSG_PING = 0x04
MSG_PONG = 0x05
MSG_CMD = 0x06
MSG_STATUS = 0x07
MSG_RELEASE = 0x08
MSG_DISCOVER = 0x09
MSG_KEEPALIVE = 0x0A

# 0x10~0x1F 预留给广域网中继/会话控制（见 docs/wan.md）

# 端口约定
DEFAULT_AGENT_PORT = 8765
DEFAULT_DEVICE_PORT = 8766

MSG_NAMES = {
    MSG_HELLO: "HELLO",
    MSG_KEY: "KEY",
    MSG_MOUSE: "MOUSE",
    MSG_PING: "PING",
    MSG_PONG: "PONG",
    MSG_CMD: "CMD",
    MSG_STATUS: "STATUS",
    MSG_RELEASE: "RELEASE",
    MSG_DISCOVER: "DISCOVER",
    MSG_KEEPALIVE: "KEEPALIVE",
}

# HID usage pages
PAGE_KEYBOARD = 0x07
PAGE_CONSUMER = 0x0C

# CMD 取值
CMD_RESYNC = 1
CMD_RELEASE_ALL = 2
CMD_SCAN = 3

# 修饰键 usage（page 0x07）
MOD_USAGES = (0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7)

_HEADER = struct.Struct("<4BHBBQIB")  # 21 bytes
assert _HEADER.size == HEADER_SIZE

# MOUSE flags
MOUSE_ABS = 0x01


class ProtocolError(ValueError):
    """收到的帧不合法。"""


@dataclass(frozen=True)
class Frame:
    version: int
    msg_type: int
    seq: int
    flags: int
    token: int
    dev_ms: int
    payload: bytes

    @property
    def type_name(self) -> str:
        return MSG_NAMES.get(self.msg_type, f"0x{self.msg_type:02X}")


@dataclass(frozen=True)
class KeyState:
    """键盘/消费类按键的全量快照。"""

    page: int
    mods: int = 0
    usages: tuple[int, ...] = ()

    @property
    def modifier_usages(self) -> tuple[int, ...]:
        if self.page != PAGE_KEYBOARD:
            return ()
        return tuple(u for bit, u in enumerate(MOD_USAGES) if self.mods & (1 << bit))


@dataclass(frozen=True)
class MouseState:
    buttons: int = 0
    absolute: bool = False
    dx: int = 0
    dy: int = 0
    wheel: int = 0
    pan: int = 0


@dataclass(frozen=True)
class Hello:
    proto_major: int
    fw_major: int
    fw_minor: int
    caps: int
    mac: bytes
    name: str


@dataclass(frozen=True)
class DeviceStatus:
    cls: int
    battery: int
    rssi: int


def encode(
    msg_type: int,
    seq: int,
    token: int,
    payload: bytes = b"",
    *,
    dev_ms: int = 0,
    flags: int = 0,
) -> bytes:
    if not 0 <= msg_type <= 0xFF:
        raise ValueError("msg_type out of range")
    if len(payload) > MAX_PAYLOAD:
        raise ValueError("payload too large")
    header = _HEADER.pack(
        MAGIC0,
        MAGIC1,
        VERSION,
        msg_type,
        seq & 0xFFFF,
        flags & 0xFF,
        0,
        token & 0xFFFFFFFFFFFFFFFF,
        dev_ms & 0xFFFFFFFF,
        len(payload),
    )
    return header + payload


def decode(data: bytes) -> Frame:
    if len(data) < HEADER_SIZE:
        raise ProtocolError(f"frame too short: {len(data)}")
    (m0, m1, version, msg_type, seq, flags, _reserved, token, dev_ms, plen) = _HEADER.unpack_from(data, 0)
    if m0 != MAGIC0 or m1 != MAGIC1:
        raise ProtocolError("bad magic")
    if version != VERSION:
        raise ProtocolError(f"unsupported version {version}")
    if len(data) != HEADER_SIZE + plen:
        raise ProtocolError(f"length mismatch: header says {plen}, got {len(data) - HEADER_SIZE}")
    return Frame(
        version=version,
        msg_type=msg_type,
        seq=seq,
        flags=flags,
        token=token,
        dev_ms=dev_ms,
        payload=bytes(data[HEADER_SIZE:]),
    )


# --------------------------------------------------------------------------
# 负载构造/解析
# --------------------------------------------------------------------------

def hello_payload(
    proto_major: int = 1,
    fw_major: int = 0,
    fw_minor: int = 1,
    caps: int = 0x07,
    mac: bytes = b"\x00" * 6,
    name: str = "esp32s3-kvm",
) -> bytes:
    if len(mac) != 6:
        raise ValueError("mac must be 6 bytes")
    return struct.pack("<BBBB", proto_major, fw_major, fw_minor, caps & 0xFF) + bytes(mac) + name.encode("utf-8")


def decode_hello(payload: bytes) -> Hello:
    if len(payload) < 10:
        raise ProtocolError("HELLO too short")
    proto_major, fw_major, fw_minor, caps = struct.unpack_from("<BBBB", payload, 0)
    mac = payload[4:10]
    name = payload[10:].decode("utf-8", "replace")
    return Hello(proto_major, fw_major, fw_minor, caps, mac, name)


def key_payload(state: KeyState) -> bytes:
    if len(state.usages) > 12:
        raise ValueError("too many usages")
    body = struct.pack("<BBB", state.page & 0xFF, state.mods & 0xFF, len(state.usages))
    body += b"".join(struct.pack("<H", u & 0xFFFF) for u in state.usages)
    return body


def decode_key(payload: bytes) -> KeyState:
    if len(payload) < 3:
        raise ProtocolError("KEY too short")
    page, mods, count = payload[0], payload[1], payload[2]
    if len(payload) != 3 + 2 * count:
        raise ProtocolError("KEY length mismatch")
    usages = tuple(struct.unpack_from("<H", payload, 3 + 2 * i)[0] for i in range(count))
    return KeyState(page=page, mods=mods, usages=usages)


MOUSE_STRUCT = struct.Struct("<BBhhhh")


def mouse_payload(state: MouseState) -> bytes:
    return MOUSE_STRUCT.pack(
        state.buttons & 0xFF,
        (MOUSE_ABS if state.absolute else 0),
        _clamp_i16(state.dx),
        _clamp_i16(state.dy),
        _clamp_i16(state.wheel),
        _clamp_i16(state.pan),
    )


def decode_mouse(payload: bytes) -> MouseState:
    if len(payload) < MOUSE_STRUCT.size:
        raise ProtocolError("MOUSE too short")
    buttons, flags, dx, dy, wheel, pan = MOUSE_STRUCT.unpack_from(payload, 0)
    return MouseState(
        buttons=buttons,
        absolute=bool(flags & MOUSE_ABS),
        dx=dx,
        dy=dy,
        wheel=wheel,
        pan=pan,
    )


def ping_payload(host_ms: int) -> bytes:
    return struct.pack("<I", host_ms & 0xFFFFFFFF)


def decode_ping(payload: bytes) -> int:
    if len(payload) < 4:
        raise ProtocolError("PING too short")
    return struct.unpack_from("<I", payload, 0)[0]


def pong_payload(host_ms: int, dev_ms: int) -> bytes:
    return struct.pack("<II", host_ms & 0xFFFFFFFF, dev_ms & 0xFFFFFFFF)


def decode_pong(payload: bytes) -> tuple[int, int]:
    if len(payload) < 8:
        raise ProtocolError("PONG too short")
    return struct.unpack_from("<II", payload, 0)


def cmd_payload(cmd: int, arg: int = 0) -> bytes:
    return struct.pack("<BB", cmd & 0xFF, arg & 0xFF)


def decode_cmd(payload: bytes) -> tuple[int, int]:
    if len(payload) < 2:
        raise ProtocolError("CMD too short")
    return payload[0], payload[1]


def status_payload(devices: list[DeviceStatus]) -> bytes:
    body = struct.pack("<B", len(devices) & 0xFF)
    for d in devices:
        body += struct.pack("<BBb", d.cls & 0xFF, d.battery & 0xFF, max(-128, min(127, d.rssi)))
    return body


def decode_status(payload: bytes) -> list[DeviceStatus]:
    if not payload:
        raise ProtocolError("STATUS empty")
    count = payload[0]
    if len(payload) != 1 + 3 * count:
        raise ProtocolError("STATUS length mismatch")
    out = []
    for i in range(count):
        cls, battery, rssi = struct.unpack_from("<BBb", payload, 1 + 3 * i)
        out.append(DeviceStatus(cls, battery, rssi))
    return out


def parse_token(text: str) -> int:
    """把配置里的 token 解析成 u64。要求 16 位十六进制。"""
    t = text.strip()
    if t.lower().startswith("0x"):
        t = t[2:]
    if len(t) != 16:
        raise ValueError("token 必须是 16 位十六进制（例如 0123456789abcdef）")
    try:
        return int(t, 16)
    except ValueError as exc:
        raise ValueError("token 必须是 16 位十六进制") from exc


def _clamp_i16(value: int) -> int:
    return max(-32768, min(32767, int(value)))
