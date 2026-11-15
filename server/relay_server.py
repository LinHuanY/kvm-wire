#!/usr/bin/env python3
"""IPKVM 广域网中继服务器（TLS，单文件，无第三方依赖）。

设备（ESP32）和 PC 注入端都主动出站连到这里，由服务器按 device_id 配对并转发
KVM-Wire 帧。服务器不解析键鼠内容。

启动：python3 relay_server.py [config.json]
"""

from __future__ import annotations

import asyncio
import json
import logging
import os
import socket
import ssl
import struct
import sys
from pathlib import Path

LOG = logging.getLogger("relay")

# 控制帧类型
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
AUTH_TIMEOUT = 10.0
IDLE_TIMEOUT = 90.0


class Conn:
    def __init__(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.reader = reader
        self.writer = writer
        self.role = ""
        self.device_id = ""
        self.name = ""
        self.lock = asyncio.Lock()
        self.closed = False
        # 键鼠帧很小，禁用 Nagle，避免 40ms 级的延迟
        sock = writer.get_extra_info("socket")
        if sock is not None:
            try:
                sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError:
                pass

    @property
    def peer_name(self) -> str:
        try:
            host, port = self.writer.get_extra_info("peername")[:2]
            return f"{host}:{port}"
        except Exception:  # noqa: BLE001
            return "?"

    async def send(self, msg_type: int, payload: bytes = b"") -> None:
        if self.closed:
            return
        body = bytes([msg_type]) + payload
        async with self.lock:
            try:
                self.writer.write(struct.pack(">I", len(body)) + body)
                await self.writer.drain()
            except (ConnectionError, asyncio.IncompleteReadError):
                self.closed = True

    async def send_json(self, msg_type: int, obj: dict) -> None:
        await self.send(msg_type, json.dumps(obj, ensure_ascii=False).encode())

    async def recv(self) -> tuple[int, bytes] | None:
        try:
            header = await asyncio.wait_for(self.reader.readexactly(4), timeout=IDLE_TIMEOUT)
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError):
            return None
        (length,) = struct.unpack(">I", header)
        if length < 1 or length > MAX_FRAME:
            return None
        try:
            body = await asyncio.wait_for(self.reader.readexactly(length), timeout=IDLE_TIMEOUT)
        except (asyncio.IncompleteReadError, asyncio.TimeoutError, ConnectionError):
            return None
        return body[0], body[1:]

    def close(self) -> None:
        if self.closed:
            return
        self.closed = True
        try:
            self.writer.close()
        except Exception:  # noqa: BLE001
            pass


class Relay:
    def __init__(self, cfg: dict) -> None:
        self.cfg = cfg
        self.device_key = cfg["device_key"]
        self.client_key = cfg["client_key"]
        self.devices: dict[str, Conn] = {}
        self.pairs: dict[str, dict[str, Conn]] = {}

    # ------------------------------------------------------------------
    async def handle(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        conn = Conn(reader, writer)
        LOG.info("新连接 %s", conn.peer_name)
        try:
            first = await asyncio.wait_for(self._read_frame(conn), timeout=AUTH_TIMEOUT)
            if first is None:
                return
            msg_type, payload = first
            if msg_type == T_HELLO_DEVICE:
                await self._on_device(conn, payload)
            elif msg_type == T_HELLO_CLIENT:
                await self._on_client(conn, payload)
            else:
                await conn.send_json(T_ERROR, {"msg": "first frame must be HELLO"})
        except (asyncio.TimeoutError, asyncio.IncompleteReadError):
            LOG.warning("认证超时 %s", conn.peer_name)
        except Exception:  # noqa: BLE001
            LOG.exception("连接异常 %s", conn.peer_name)
        finally:
            await self._cleanup(conn)

    async def _read_frame(self, conn: Conn):
        try:
            header = await conn.reader.readexactly(4)
        except (asyncio.IncompleteReadError, ConnectionError):
            return None
        (length,) = struct.unpack(">I", header)
        if length < 1 or length > MAX_FRAME:
            return None
        body = await conn.reader.readexactly(length)
        return body[0], body[1:]

    # ------------------------------------------------------------------
    async def _on_device(self, conn: Conn, payload: bytes) -> None:
        try:
            info = json.loads(payload)
        except json.JSONDecodeError:
            await conn.send_json(T_ERROR, {"msg": "bad json"})
            return
        if info.get("key") != self.device_key:
            await conn.send_json(T_ERROR, {"msg": "bad device key"})
            LOG.warning("设备密钥错误 %s", conn.peer_name)
            return
        device_id = str(info.get("device_id") or "").lower()
        if not device_id:
            await conn.send_json(T_ERROR, {"msg": "missing device_id"})
            return
        old = self.devices.get(device_id)
        if old and old is not conn:
            old.close()
        conn.role = "device"
        conn.device_id = device_id
        conn.name = str(info.get("name") or device_id)
        self.devices[device_id] = conn
        await conn.send_json(T_HELLO_OK, {"device_id": device_id})
        LOG.info("设备上线 %s (%s) %s", device_id, conn.name, conn.peer_name)

        paired = self.pairs.get(device_id, {}).get("client")
        if paired:
            await self._pair(device_id, conn, paired)

        while True:
            item = await conn.recv()
            if item is None:
                break
            msg_type, body = item
            if msg_type == T_DATA:
                client = self.pairs.get(device_id, {}).get("client")
                if client:
                    await client.send(T_DATA, body)
            elif msg_type == T_PING:
                await conn.send(T_PONG)
            elif msg_type == T_BYE:
                break

    async def _on_client(self, conn: Conn, payload: bytes) -> None:
        try:
            info = json.loads(payload)
        except json.JSONDecodeError:
            await conn.send_json(T_ERROR, {"msg": "bad json"})
            return
        if info.get("key") != self.client_key:
            await conn.send_json(T_ERROR, {"msg": "bad client key"})
            LOG.warning("客户端密钥错误 %s", conn.peer_name)
            return
        conn.role = "client"
        await conn.send_json(T_HELLO_OK, {"role": "client"})
        await self._send_device_list(conn)
        LOG.info("客户端上线 %s", conn.peer_name)

        while True:
            item = await conn.recv()
            if item is None:
                break
            msg_type, body = item
            if msg_type == T_CONNECT:
                try:
                    req = json.loads(body)
                except json.JSONDecodeError:
                    await conn.send_json(T_ERROR, {"msg": "bad connect"})
                    continue
                device_id = str(req.get("device_id") or "").lower()
                device = self.devices.get(device_id)
                if not device:
                    await conn.send_json(T_ERROR, {"msg": "device offline"})
                    continue
                await self._pair(device_id, device, conn)
            elif msg_type == T_DATA:
                device_id = self._client_device(conn)
                if device_id:
                    device = self.devices.get(device_id)
                    if device:
                        await device.send(T_DATA, body)
            elif msg_type == T_PING:
                await conn.send(T_PONG)
            elif msg_type == T_BYE:
                break

    def _client_device(self, conn: Conn) -> str | None:
        for device_id, pair in self.pairs.items():
            if pair.get("client") is conn:
                return device_id
        return None

    async def _pair(self, device_id: str, device: Conn, client: Conn) -> None:
        old = self.pairs.get(device_id)
        if old and old.get("client") is not client:
            await old["client"].send_json(T_ERROR, {"msg": "session replaced"})
            old["client"].close()
        self.pairs[device_id] = {"device": device, "client": client}
        await device.send_json(T_PEER_READY, {"role": "device", "device_id": device_id})
        await client.send_json(T_PEER_READY, {"role": "client", "device_id": device_id})
        LOG.info("配对成功 %s", device_id)

    async def _send_device_list(self, conn: Conn) -> None:
        items = [
            {"device_id": d, "name": c.name, "online": True}
            for d, c in self.devices.items()
            if not c.closed
        ]
        await conn.send_json(T_DEVICE_LIST, {"devices": items})

    async def _cleanup(self, conn: Conn) -> None:
        conn.close()
        if conn.role == "device" and self.devices.get(conn.device_id) is conn:
            del self.devices[conn.device_id]
            pair = self.pairs.pop(conn.device_id, None)
            if pair and pair.get("client"):
                await pair["client"].send_json(T_PEER_GONE, {"device_id": conn.device_id})
            LOG.info("设备下线 %s", conn.device_id)
        elif conn.role == "client":
            device_id = self._client_device(conn)
            if device_id:
                self.pairs.pop(device_id, None)
                device = self.devices.get(device_id)
                if device:
                    await device.send_json(T_PEER_GONE, {"device_id": device_id})
            LOG.info("客户端下线 %s", conn.peer_name)


async def main() -> None:
    base = Path(__file__).resolve().parent
    cfg_path = Path(sys.argv[1]) if len(sys.argv) > 1 else base / "config.json"
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))

    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.minimum_version = ssl.TLSVersion.TLSv1_2
    ctx.load_cert_chain(base / cfg["cert"], base / cfg["key"])

    relay = Relay(cfg)
    server = await asyncio.start_server(
        relay.handle, cfg.get("listen", "0.0.0.0"), int(cfg["port"]), ssl=ctx
    )
    addrs = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
    LOG.info("IPKVM relay listening on %s (TLS)", addrs)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
