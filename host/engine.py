"""输入状态机：把协议事件翻译成注入器调用。

这一层不依赖 Windows，方便单元测试：注入器只要实现
key()/move()/button()/wheel()/hwheel() 即可。
"""

from __future__ import annotations

import threading
import time
from dataclasses import dataclass, field
from typing import Protocol

import hidmap
import kvmproto


class Sink(Protocol):
    def key(self, vk: int, down: bool, extended: bool = False) -> bool: ...
    def move(self, dx: float, dy: float) -> bool: ...
    def button(self, index: int, down: bool) -> bool: ...
    def wheel(self, steps: int) -> bool: ...
    def hwheel(self, steps: int) -> bool: ...


@dataclass
class EngineConfig:
    repeat: bool = True
    repeat_delay: float = 0.50
    repeat_interval: float = 0.033
    mouse_scale: float = 1.0
    wheel_scale: int = 1
    watchdog_timeout: float = 3.0
    verbose: bool = False


@dataclass
class Stats:
    frames: int = 0
    keys: int = 0
    mouse: int = 0
    hello: int = 0
    status: int = 0
    bad: int = 0
    lost: int = 0
    last_seq: int | None = None
    last_rx: float = 0.0
    started: float = field(default_factory=time.monotonic)
    last_latency_ms: float = 0.0

    def pps(self, now: float | None = None) -> float:
        now = now if now is not None else time.monotonic()
        elapsed = max(1e-6, now - self.started)
        return self.frames / elapsed


class InputEngine:
    """维护"期望状态"，只在状态变化时向注入器发事件。"""

    def __init__(self, sink: Sink, cfg: EngineConfig | None = None) -> None:
        self.sink = sink
        self.cfg = cfg or EngineConfig()
        self.stats = Stats()
        self._lock = threading.RLock()

        self._held: dict[int, tuple[int, bool]] = {}       # keyboard page usage -> (vk, ext)
        self._repeat_at: dict[int, float] = {}
        self._consumer_held: set[int] = set()
        self._buttons_down: set[int] = set()
        self._link_lost_reported = False
        self._device_name: str | None = None
        self._rx_seen = False

    # ------------------------------------------------------------------
    # 入口
    # ------------------------------------------------------------------
    def handle_frame(self, frame: kvmproto.Frame, now: float | None = None) -> None:
        now = now if now is not None else time.monotonic()
        with self._lock:
            self.stats.frames += 1
            self.stats.last_rx = now
            self._rx_seen = True
            self._link_lost_reported = False
            self._track_seq(frame.seq)

            if frame.msg_type == kvmproto.MSG_KEY:
                self._on_key(kvmproto.decode_key(frame.payload), now)
            elif frame.msg_type == kvmproto.MSG_MOUSE:
                self._on_mouse(kvmproto.decode_mouse(frame.payload), now)
            elif frame.msg_type == kvmproto.MSG_HELLO:
                self._on_hello(kvmproto.decode_hello(frame.payload))
            elif frame.msg_type == kvmproto.MSG_STATUS:
                self._on_status(kvmproto.decode_status(frame.payload))
            elif frame.msg_type == kvmproto.MSG_RELEASE:
                self.release_all("device requested release")
            # PING/PONG 由 agent 处理，这里计数即可

    def tick(self, now: float | None = None) -> None:
        now = now if now is not None else time.monotonic()
        with self._lock:
            if self.cfg.repeat:
                self._do_repeat(now)
            self._check_watchdog(now)

    # ------------------------------------------------------------------
    # 键盘
    # ------------------------------------------------------------------
    def _on_key(self, state: kvmproto.KeyState, now: float) -> None:
        self.stats.keys += 1
        if state.page == kvmproto.PAGE_CONSUMER:
            self._apply_consumer(state.usages)
            return
        if state.page != kvmproto.PAGE_KEYBOARD:
            if self.cfg.verbose:
                print(f"[key] 忽略未知 page 0x{state.page:02X} usages={list(state.usages)}")
            return

        target_usages = {u for u in state.usages if u not in (0x00, 0x01)}
        target_usages.update(state.modifier_usages)

        if self.cfg.verbose:
            names = ",".join(hidmap.usage_name(u) for u in sorted(target_usages))
            print(f"[key] mods=0x{state.mods:02X} held=[{names}]")

        target: dict[int, tuple[int, bool]] = {}
        for usage in target_usages:
            mapped = hidmap.key_usage_to_vk(usage)
            if mapped is None:
                if self.cfg.verbose:
                    print(f"[key] 未映射 usage 0x{usage:02X}，忽略")
                continue
            target[usage] = mapped

        # 释放：先普通键，最后修饰键，避免 Ctrl 先抬起导致组合键语义变化
        for usage in sorted(set(self._held) - set(target), key=lambda u: hidmap.is_modifier_usage(u)):
            vk, ext = self._held.pop(usage)
            self._repeat_at.pop(usage, None)
            self.sink.key(vk, False, ext)

        # 按下：先修饰键
        for usage in sorted(set(target) - set(self._held), key=lambda u: not hidmap.is_modifier_usage(u)):
            vk, ext = target[usage]
            self.sink.key(vk, True, ext)
            self._held[usage] = (vk, ext)
            if self._should_repeat(usage):
                self._repeat_at[usage] = now + self.cfg.repeat_delay

    def _should_repeat(self, usage: int) -> bool:
        if usage >= 0xE0:  # 修饰键
            return False
        if usage in (0x39, 0x53, 0x47):  # CapsLock / NumLock / ScrollLock
            return False
        return True

    def _do_repeat(self, now: float) -> None:
        for usage in list(self._repeat_at):
            if usage not in self._held:
                self._repeat_at.pop(usage, None)
                continue
            if now < self._repeat_at[usage]:
                continue
            vk, ext = self._held[usage]
            self.sink.key(vk, True, ext)
            self._repeat_at[usage] = now + self.cfg.repeat_interval

    def _apply_consumer(self, usages: tuple[int, ...]) -> None:
        current = {u for u in usages if u}
        for usage in current - self._consumer_held:
            vk = hidmap.consumer_usage_to_vk(usage)
            if vk is None:
                if self.cfg.verbose:
                    print(f"[consumer] 未映射 usage 0x{usage:04X}")
                continue
            # 消费类按键是瞬时的：按下+抬起
            self.sink.key(vk, True, False)
            self.sink.key(vk, False, False)
        self._consumer_held = current

    # ------------------------------------------------------------------
    # 鼠标
    # ------------------------------------------------------------------
    def _on_mouse(self, state: kvmproto.MouseState, now: float) -> None:
        self.stats.mouse += 1
        scale = self.cfg.mouse_scale

        if state.absolute:
            # 绝对坐标交给注入器按 0..32767 比例处理（本版本仅记录）
            if self.cfg.verbose:
                print(f"[mouse] abs ({state.dx},{state.dy}) buttons=0x{state.buttons:02X}")
        elif state.dx or state.dy:
            self.sink.move(state.dx * scale, state.dy * scale)

        for index in range(5):
            bit = 1 << index
            down = bool(state.buttons & bit)
            if down and index not in self._buttons_down:
                self.sink.button(index, True)
                self._buttons_down.add(index)
            elif not down and index in self._buttons_down:
                self.sink.button(index, False)
                self._buttons_down.discard(index)

        if state.wheel:
            self.sink.wheel(state.wheel * self.cfg.wheel_scale)
        if state.pan:
            self.sink.hwheel(state.pan * self.cfg.wheel_scale)

    # ------------------------------------------------------------------
    # 其它
    # ------------------------------------------------------------------
    def _on_hello(self, hello: kvmproto.Hello) -> None:
        self.stats.hello += 1
        caps = []
        if hello.caps & 1:
            caps.append("keyboard")
        if hello.caps & 2:
            caps.append("mouse")
        if hello.caps & 4:
            caps.append("consumer")
        self._device_name = hello.name
        print(
            f"[link] 设备上线: {hello.name!r} MAC={hello.mac.hex(':')} "
            f"fw={hello.fw_major}.{hello.fw_minor} caps={','.join(caps) or '-'}"
        )

    def _on_status(self, devices: list[kvmproto.DeviceStatus]) -> None:
        self.stats.status += 1
        names = {1: "键盘", 2: "鼠标", 3: "消费"}
        parts = []
        for d in devices:
            battery = "?" if d.battery == 255 else f"{d.battery}%"
            parts.append(f"{names.get(d.cls, d.cls)} 电量={battery} rssi={d.rssi}dBm")
        if parts:
            print("[ble] " + " | ".join(parts))

    def _track_seq(self, seq: int) -> None:
        if self.stats.last_seq is not None:
            expected = (self.stats.last_seq + 1) & 0xFFFF
            if seq != expected:
                gap = (seq - expected) & 0xFFFF
                # 只统计"向前跳过"的丢包，忽略重排/重复
                if 0 < gap < 0x8000:
                    self.stats.lost += gap
        self.stats.last_seq = seq

    def _check_watchdog(self, now: float) -> None:
        if not self._rx_seen:
            return
        idle = now - self.stats.last_rx
        if idle <= self.cfg.watchdog_timeout:
            return
        if self._held or self._buttons_down or self._consumer_held:
            print(f"[watchdog] {idle:.1f}s 没有收到设备数据，释放全部按键")
            self.release_all("watchdog")
        elif not self._link_lost_reported:
            print(f"[watchdog] 链路可能已断开（{idle:.1f}s 无数据）")
            self._link_lost_reported = True

    def release_all(self, reason: str = "") -> None:
        with self._lock:
            for usage in sorted(self._held, key=lambda u: not hidmap.is_modifier_usage(u)):
                vk, ext = self._held[usage]
                self.sink.key(vk, False, ext)
            self._held.clear()
            self._repeat_at.clear()
            self._consumer_held.clear()
            for index in sorted(self._buttons_down):
                self.sink.button(index, False)
            self._buttons_down.clear()
        if reason:
            print(f"[safety] 已释放全部输入（{reason}）")


class PrintSink:
    """dry-run 注入器：打印事件，鼠标位移做聚合统计。"""

    def __init__(self) -> None:
        self.moves = 0
        self.dx = 0.0
        self.dy = 0.0
        self.last_report = time.monotonic()

    def key(self, vk: int, down: bool, extended: bool = False) -> bool:
        print(f"  KEY {'DOWN' if down else 'UP  '} vk=0x{vk:02X}{' ext' if extended else ''}")
        return True

    def move(self, dx: float, dy: float) -> bool:
        self.moves += 1
        self.dx += dx
        self.dy += dy
        return True

    def button(self, index: int, down: bool) -> bool:
        print(f"  MOUSE btn{index} {'DOWN' if down else 'UP'}")
        return True

    def wheel(self, steps: int) -> bool:
        print(f"  WHEEL {steps:+d}")
        return True

    def hwheel(self, steps: int) -> bool:
        print(f"  HWHEEL {steps:+d}")
        return True

    def tick(self, now: float | None = None) -> None:
        now = now if now is not None else time.monotonic()
        if now - self.last_report < 1.0:
            return
        if self.moves:
            print(f"  MOUSE move x{self.moves} 累计 dx={self.dx:+.0f} dy={self.dy:+.0f}")
        self.moves = 0
        self.dx = 0.0
        self.dy = 0.0
        self.last_report = now


class NullSink:
    def key(self, vk: int, down: bool, extended: bool = False) -> bool:
        return True

    def move(self, dx: float, dy: float) -> bool:
        return True

    def button(self, index: int, down: bool) -> bool:
        return True

    def wheel(self, steps: int) -> bool:
        return True

    def hwheel(self, steps: int) -> bool:
        return True
