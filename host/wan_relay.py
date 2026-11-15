"""IPKVM 广域网中继客户端（设备端 / PC 端共用帧协议）。

TLS + 长度前缀帧，控制消息 JSON。用法见 wan_selftest.py。
"""

from __future__ import annotations

import json
import queue
import socket
import ssl
import struct
import threading
import time

T_HELLO_DEVICE = 1
T_HELLO_CLIENT = 2
T_HELLO_OK = 3
T_DEVICE_LIST = 4
T_CONNECT = 5
T_PEER_READY = 6
T_DATA = 7
T_PING = 8
T_PONG = 9
T_ERROR = 10
T_PEER_GONE = 11
T_BYE = 12

MAX_FRAME = 8192


class RelayError(Exception):
    pass


def _send_frame(sock: ssl.SSLSocket, msg_type: int, payload: bytes = b"") -> None:
    body = bytes([msg_type]) + payload
    sock.sendall(struct.pack(">I", len(body)) + body)


def _send_json(sock: ssl.SSLSocket, msg_type: int, obj: dict) -> None:
    _send_frame(sock, msg_type, json.dumps(obj, ensure_ascii=False).encode())


def _recv_exact(sock: ssl.SSLSocket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("relay closed")
        buf += chunk
    return buf


def _recv_frame(sock: ssl.SSLSocket) -> tuple[int, bytes]:
    (length,) = struct.unpack(">I", _recv_exact(sock, 4))
    if length < 1 or length > MAX_FRAME:
        raise RelayError(f"bad frame length {length}")
    body = _recv_exact(sock, length)
    return body[0], body[1:]


def _make_ctx(ca_file: str) -> ssl.SSLContext:
    ctx = ssl.create_default_context(cafile=ca_file)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    return ctx


class _Base:
    def __init__(self, host: str, port: int, ca_file: str) -> None:
        self.host = host
        self.port = port
        self.ca_file = ca_file
        self.sock: ssl.SSLSocket | None = None
        self._rx: queue.Queue[bytes] = queue.Queue()
        self._stop = threading.Event()
        self._thread: threading.Thread | None = None
        self.peer_ready = threading.Event()
        self.peer_gone = threading.Event()
        self.last_error: str | None = None
        self._send_lock = threading.Lock()

    # -- 子类实现 ------------------------------------------------------
    def _hello(self) -> None:
        raise NotImplementedError

    def _on_control(self, msg_type: int, payload: bytes) -> None:
        pass

    # ------------------------------------------------------------------
    def _open(self, timeout: float) -> ssl.SSLSocket:
        raw = socket.create_connection((self.host, self.port), timeout=timeout)
        raw.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        ctx = _make_ctx(self.ca_file)
        sock = ctx.wrap_socket(raw, server_hostname=self.host)
        sock.settimeout(None)
        return sock

    def connect(self, timeout: float = 10.0) -> None:
        self.sock = self._open(timeout)
        self._hello()
        msg_type, payload = _recv_frame(self.sock)
        if msg_type == T_ERROR:
            raise RelayError(json.loads(payload).get("msg", "relay error"))
        if msg_type != T_HELLO_OK:
            raise RelayError(f"unexpected hello reply {msg_type}")
        self._thread = threading.Thread(target=self._rx_loop, name="relay-rx", daemon=True)
        self._thread.start()

    def _rx_loop(self) -> None:
        assert self.sock is not None
        try:
            while not self._stop.is_set():
                msg_type, payload = _recv_frame(self.sock)
                if msg_type == T_DATA:
                    self._rx.put(payload)
                elif msg_type == T_PEER_READY:
                    self.peer_ready.set()
                elif msg_type == T_PEER_GONE:
                    self.peer_gone.set()
                    self.last_error = "peer gone"
                elif msg_type == T_PING:
                    with self._send_lock:
                        _send_frame(self.sock, T_PONG)
                elif msg_type == T_ERROR:
                    self.last_error = json.loads(payload).get("msg", "error")
                else:
                    self._on_control(msg_type, payload)
        except (ConnectionError, OSError, RelayError):
            self.last_error = self.last_error or "connection closed"
        finally:
            self._stop.set()

    def send(self, frame: bytes) -> None:
        if self.sock is None:
            raise RelayError("not connected")
        with self._send_lock:
            _send_frame(self.sock, T_DATA, frame)

    def recv(self, timeout: float = 0.5) -> bytes | None:
        try:
            return self._rx.get(timeout=timeout)
        except queue.Empty:
            return None

    def close(self) -> None:
        self._stop.set()
        if self.sock is not None:
            try:
                _send_frame(self.sock, T_BYE)
            except OSError:
                pass
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

    @property
    def connected(self) -> bool:
        return self.sock is not None and not self._stop.is_set() and not self.last_error


class RelayDevice(_Base):
    """模拟 ESP32 设备注册到中继。"""

    def __init__(self, host: str, port: int, ca_file: str, device_key: str,
                 device_id: str, name: str = "esp32s3-kvm") -> None:
        super().__init__(host, port, ca_file)
        self.device_key = device_key
        self.device_id = device_id
        self.name = name

    def _hello(self) -> None:
        assert self.sock is not None
        _send_json(self.sock, T_HELLO_DEVICE, {
            "key": self.device_key, "device_id": self.device_id, "name": self.name,
        })

    def wait_peer(self, timeout: float = 30.0) -> bool:
        return self.peer_ready.wait(timeout)


class RelayClient(_Base):
    """PC 注入端：列出设备并连接其中一个。"""

    def __init__(self, host: str, port: int, ca_file: str, client_key: str) -> None:
        super().__init__(host, port, ca_file)
        self.client_key = client_key
        self.devices: list[dict] = []
        self.device_id: str | None = None
        self.devices_ready = threading.Event()

    def _hello(self) -> None:
        assert self.sock is not None
        _send_json(self.sock, T_HELLO_CLIENT, {"key": self.client_key})

    def _on_control(self, msg_type: int, payload: bytes) -> None:
        if msg_type == T_DEVICE_LIST:
            self.devices = json.loads(payload).get("devices", [])
            self.devices_ready.set()

    def connect(self, timeout: float = 10.0) -> None:
        super().connect(timeout)
        # DEVICE_LIST 紧跟在 HELLO_OK 后面，等接收线程把它处理完
        self.devices_ready.wait(1.5)

    def open_device(self, device_id: str, timeout: float = 10.0) -> bool:
        assert self.sock is not None
        self.device_id = device_id
        _send_json(self.sock, T_CONNECT, {"device_id": device_id})
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.peer_ready.is_set():
                return True
            if self.last_error:
                return False
            time.sleep(0.05)
        return False
