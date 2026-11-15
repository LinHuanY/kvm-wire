"""中继链路自测：模拟设备 + PC 客户端，经公网服务器交换 KVM 帧。

用法：
    python wan_selftest.py --host relay.example.com --port 8790 \
        --ca ..\\server\\ca.crt --device-key ... --client-key ...
"""

from __future__ import annotations

import argparse
import sys
import time

import kvmproto
import wan_relay


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", required=True)
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--ca", required=True)
    ap.add_argument("--device-key", required=True)
    ap.add_argument("--client-key", required=True)
    ap.add_argument("--device-id", default="aabbccddeeff")
    ap.add_argument("--keep", type=float, default=0.0, help="成功后保持连接的秒数")
    args = ap.parse_args()

    dev = wan_relay.RelayDevice(args.host, args.port, args.ca, args.device_key,
                                args.device_id, "selftest-device")
    cli = wan_relay.RelayClient(args.host, args.port, args.ca, args.client_key)
    try:
        t0 = time.monotonic()
        dev.connect()
        print(f"[ok] device registered in {(time.monotonic()-t0)*1000:.0f} ms")

        t0 = time.monotonic()
        cli.connect()
        print(f"[ok] client auth in {(time.monotonic()-t0)*1000:.0f} ms, devices={cli.devices}")

        if not cli.devices or cli.devices[0]["device_id"] != args.device_id:
            print("[fail] device not listed")
            return 2

        t0 = time.monotonic()
        if not cli.open_device(args.device_id, timeout=10):
            print("[fail] pair failed:", cli.last_error)
            return 3
        print(f"[ok] paired in {(time.monotonic()-t0)*1000:.0f} ms")

        hello = kvmproto.hello_payload(mac=bytes.fromhex(args.device_id), name="selftest-device")
        frame = kvmproto.encode(kvmproto.MSG_HELLO, 1, 0x0123456789ABCDEF, hello)
        dev.send(frame)
        got = cli.recv(3.0)
        if got != frame:
            print("[fail] client did not receive device frame")
            return 4
        print(f"[ok] device -> client frame, {len(frame)} bytes")

        ping = kvmproto.encode(kvmproto.MSG_PING, 2, 0x0123456789ABCDEF, kvmproto.ping_payload(123))
        cli.send(ping)
        got = dev.recv(3.0)
        if got != ping:
            print("[fail] device did not receive client frame")
            return 5
        print(f"[ok] client -> device frame, {len(ping)} bytes")

        if args.keep:
            print(f"[..] keeping alive {args.keep:.0f}s")
            time.sleep(args.keep)
        return 0
    finally:
        cli.close()
        dev.close()


if __name__ == "__main__":
    raise SystemExit(main())
