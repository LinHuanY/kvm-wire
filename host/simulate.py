#!/usr/bin/env python3
"""模拟 ESP32 发送键鼠事件，用来在没有硬件时验证 PC 注入端。

示例：
    python kvm_agent.py --dry-run --token 0123456789abcdef
    python simulate.py --token 0123456789abcdef --demo

    # 只打字
    python simulate.py --token 0123456789abcdef --text "hello world"

    # 交互式
    python simulate.py --token 0123456789abcdef --interactive
"""

from __future__ import annotations

import argparse
import math
import socket
import sys
import threading
import time

import kvmproto
from kvmproto import KeyState, MouseState

KEYMAP = {}
for _i in range(26):
    KEYMAP[chr(ord("a") + _i)] = 0x04 + _i
for _i, _ch in enumerate("1234567890"):
    KEYMAP[_ch] = 0x1E + _i
KEYMAP.update({
    " ": 0x2C, "\n": 0x28, "\t": 0x2B, "-": 0x2D, "=": 0x2E, "[": 0x2F, "]": 0x30,
    "\\": 0x31, ";": 0x33, "'": 0x34, "`": 0x35, ",": 0x36, ".": 0x37, "/": 0x38,
})
SHIFT_MAP = {
    "!": "1", "@": "2", "#": "3", "$": "4", "%": "5", "^": "6", "&": "7", "*": "8",
    "(": "9", ")": "0", "_": "-", "+": "=", "{": "[", "}": "]", "|": "\\", ":": ";",
    '"': "'", "~": "`", "<": ",", ">": ".", "?": "/",
}
MOD_LSHIFT = 1 << 1


class Sim:
    def __init__(self, host: str, port: int, token: int, device_port: int = kvmproto.DEFAULT_DEVICE_PORT) -> None:
        self.addr = (host, port)
        self.token = token
        self.seq = 0
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.device_port = device_port
        try:
            self.sock.bind(("0.0.0.0", device_port))
        except OSError:
            print(f"[sim] 端口 {device_port} 被占用，改用随机端口（自动发现将不可用）")
            self.sock.bind(("0.0.0.0", 0))
            self.device_port = self.sock.getsockname()[1]
        self.sock.settimeout(0.2)
        self.start = time.monotonic()
        self._pings = 0
        self._stop = threading.Event()
        self._rx = threading.Thread(target=self._rx_loop, name="sim-rx", daemon=True)
        self._rx.start()

    def dev_ms(self) -> int:
        return int((time.monotonic() - self.start) * 1000)

    def send(self, msg_type: int, payload: bytes) -> None:
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(kvmproto.encode(msg_type, self.seq, self.token, payload, dev_ms=self.dev_ms()), self.addr)

    def _send_to(self, msg_type: int, payload: bytes, addr: tuple[str, int]) -> None:
        self.seq = (self.seq + 1) & 0xFFFF
        self.sock.sendto(kvmproto.encode(msg_type, self.seq, self.token, payload, dev_ms=self.dev_ms()), addr)

    def _hello_payload(self) -> bytes:
        return kvmproto.hello_payload(mac=b"\xde\xad\xbe\xef\x00\x01", name="simulator-esp32s3")

    def _rx_loop(self) -> None:
        """回复 DISCOVER（模拟固件）并回 PONG，让 agent 能显示 RTT。"""
        while not self._stop.is_set():
            try:
                data, addr = self.sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                return
            try:
                frame = kvmproto.decode(data)
            except kvmproto.ProtocolError:
                continue
            if frame.msg_type == kvmproto.MSG_DISCOVER:
                self._send_to(kvmproto.MSG_HELLO, self._hello_payload(), addr)
                print(f"[sim] 回应发现请求 -> {addr[0]}:{addr[1]}")
                continue
            if frame.msg_type == kvmproto.MSG_PING and len(frame.payload) >= 4:
                host_ms = kvmproto.decode_ping(frame.payload)
                self.addr = addr
                self._send_to(kvmproto.MSG_PONG, kvmproto.pong_payload(host_ms, self.dev_ms()), addr)
                if self._pings < 3:
                    self._pings += 1
                    print(f"[sim] PING -> PONG to {addr[0]}:{addr[1]}")

    def close(self) -> None:
        self._stop.set()
        self.sock.close()

    def hello(self, name: str = "simulator-esp32s3") -> None:
        self.send(kvmproto.MSG_HELLO, kvmproto.hello_payload(mac=b"\xde\xad\xbe\xef\x00\x01", name=name))
    def keys(self, usages: list[int], mods: int = 0) -> None:
        self.send(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(kvmproto.PAGE_KEYBOARD, mods, tuple(usages))))

    def consumer(self, usage: int) -> None:
        self.send(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(kvmproto.PAGE_CONSUMER, 0, (usage,))))
        time.sleep(0.02)
        self.send(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(kvmproto.PAGE_CONSUMER, 0, ())))

    def mouse(self, buttons: int = 0, dx: int = 0, dy: int = 0, wheel: int = 0, pan: int = 0) -> None:
        self.send(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(buttons, False, dx, dy, wheel, pan)))

    def release_all(self) -> None:
        self.keys([])
        self.mouse(0)

    def type_text(self, text: str, delay: float = 0.03) -> None:
        for ch in text:
            mods = 0
            if ch in SHIFT_MAP:
                ch = SHIFT_MAP[ch]
                mods = MOD_LSHIFT
            if ch.isupper():
                ch = ch.lower()
                mods = MOD_LSHIFT
            usage = KEYMAP.get(ch)
            if usage is None:
                print(f"  (跳过无法映射的字符 {ch!r})")
                continue
            self.keys([usage], mods)
            time.sleep(delay)
            self.keys([], 0)
            time.sleep(delay)

    def demo(self) -> None:
        print("[sim] HELLO")
        self.hello()
        time.sleep(0.3)

        print("[sim] 打字: Hello KVM!")
        self.type_text("Hello KVM!")

        print("[sim] 鼠标画圆")
        for i in range(120):
            angle = i / 12.0
            self.mouse(dx=int(round(6 * math.cos(angle))), dy=int(round(6 * math.sin(angle))))
            time.sleep(0.008)

        print("[sim] 左键单击 + 滚轮 + 音量+")
        self.mouse(1)
        time.sleep(0.05)
        self.mouse(0)
        time.sleep(0.1)
        self.mouse(wheel=-3)
        time.sleep(0.2)
        self.consumer(0x00E9)
        time.sleep(0.2)
        self.release_all()
        print("[sim] 完成")


def interactive(sim: Sim) -> None:
    print("命令：type <文本> | move <dx> <dy> | click <left|right|middle> | wheel <格数> | key <usage十六进制> | quit")
    while True:
        try:
            line = input("sim> ").strip()
        except (EOFError, KeyboardInterrupt):
            break
        if not line:
            continue
        parts = line.split(maxsplit=1)
        cmd = parts[0].lower()
        arg = parts[1] if len(parts) > 1 else ""
        try:
            if cmd in ("quit", "exit"):
                break
            elif cmd == "type":
                sim.type_text(arg)
            elif cmd == "move":
                dx, dy = (int(v) for v in arg.split())
                sim.mouse(dx=dx, dy=dy)
            elif cmd == "click":
                btn = {"left": 0, "right": 1, "middle": 2}.get(arg, 0)
                sim.mouse(1 << btn)
                time.sleep(0.05)
                sim.mouse(0)
            elif cmd == "wheel":
                sim.mouse(wheel=int(arg))
            elif cmd == "key":
                usage = int(arg, 16)
                sim.keys([usage])
                time.sleep(0.05)
                sim.keys([])
            else:
                print("未知命令")
        except Exception as exc:  # noqa: BLE001
            print(f"错误: {exc}")
    sim.release_all()


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="模拟 ESP32 键鼠事件")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=8765, help="PC 注入端监听端口")
    p.add_argument("--device-port", type=int, default=kvmproto.DEFAULT_DEVICE_PORT,
                   help="模拟设备的监听端口（自动发现用），默认 8766")
    p.add_argument("--token", required=True, help="16 位十六进制")
    p.add_argument("--demo", action="store_true", help="跑一遍演示序列")
    p.add_argument("--serve", action="store_true", help="常驻，只回应 DISCOVER/PING（配合 GUI 联调）")
    p.add_argument("--text", help="只发送一段文本")
    p.add_argument("--interactive", action="store_true", help="交互式发送")
    args = p.parse_args(argv)

    sim = Sim(args.host, args.port, kvmproto.parse_token(args.token), args.device_port)
    print(f"[sim] 模拟设备监听 UDP {sim.device_port}，向 {args.host}:{args.port} 发送事件")
    try:
        if args.demo:
            sim.demo()
        elif args.serve:
            print("[sim] 常驻模式：等待 PC 的 DISCOVER / PING（Ctrl+C 退出）")
            while True:
                time.sleep(0.5)
        elif args.text is not None:
            sim.hello("simulator-text")
            sim.type_text(args.text)
        elif args.interactive:
            sim.hello()
            interactive(sim)
        else:
            sim.hello()
            sim.type_text("hello")
            print("已发送默认序列；用 --demo 或 --interactive 体验更多")
    finally:
        sim.release_all()
        sim.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
