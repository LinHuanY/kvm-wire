import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import kvmproto  # noqa: E402
from kvmproto import KeyState, MouseState  # noqa: E402


TOKEN = 0x0123456789ABCDEF


class ProtocolTest(unittest.TestCase):
    def test_roundtrip(self):
        data = kvmproto.encode(kvmproto.MSG_KEY, 7, TOKEN, kvmproto.key_payload(KeyState(7, 2, (4, 5))), dev_ms=1234)
        frame = kvmproto.decode(data)
        self.assertEqual(frame.msg_type, kvmproto.MSG_KEY)
        self.assertEqual(frame.seq, 7)
        self.assertEqual(frame.token, TOKEN)
        self.assertEqual(frame.dev_ms, 1234)
        state = kvmproto.decode_key(frame.payload)
        self.assertEqual(state.page, 7)
        self.assertEqual(state.mods, 2)
        self.assertEqual(state.usages, (4, 5))

    def test_modifier_usages(self):
        state = KeyState(page=7, mods=0b101)
        self.assertEqual(state.modifier_usages, (0xE0, 0xE2))

    def test_bad_magic(self):
        data = bytearray(kvmproto.encode(kvmproto.MSG_HELLO, 1, TOKEN))
        data[0] = 0
        with self.assertRaises(kvmproto.ProtocolError):
            kvmproto.decode(bytes(data))

    def test_bad_length(self):
        data = kvmproto.encode(kvmproto.MSG_KEY, 1, TOKEN, b"\x07\x00\x00")
        with self.assertRaises(kvmproto.ProtocolError):
            kvmproto.decode(data[:-1])

    def test_mouse_roundtrip(self):
        payload = kvmproto.mouse_payload(MouseState(buttons=3, absolute=False, dx=-12, dy=34, wheel=-1, pan=2))
        state = kvmproto.decode_mouse(payload)
        self.assertEqual(state.buttons, 3)
        self.assertEqual((state.dx, state.dy, state.wheel, state.pan), (-12, 34, -1, 2))

    def test_mouse_clamp(self):
        payload = kvmproto.mouse_payload(MouseState(dx=999999))
        self.assertEqual(kvmproto.decode_mouse(payload).dx, 32767)

    def test_hello(self):
        payload = kvmproto.hello_payload(mac=b"\x01\x02\x03\x04\x05\x06", name="测试键盘")
        hello = kvmproto.decode_hello(payload)
        self.assertEqual(hello.mac, b"\x01\x02\x03\x04\x05\x06")
        self.assertEqual(hello.name, "测试键盘")

    def test_status(self):
        payload = kvmproto.status_payload([kvmproto.DeviceStatus(1, 88, -55)])
        status = kvmproto.decode_status(payload)
        self.assertEqual(status[0].battery, 88)
        self.assertEqual(status[0].rssi, -55)

    def test_token(self):
        self.assertEqual(kvmproto.parse_token("0123456789abcdef"), TOKEN)
        self.assertEqual(kvmproto.parse_token("0x0123456789ABCDEF"), TOKEN)
        with self.assertRaises(ValueError):
            kvmproto.parse_token("abc")

    def test_ping(self):
        self.assertEqual(kvmproto.decode_ping(kvmproto.ping_payload(987654)), 987654)
        self.assertEqual(kvmproto.decode_pong(kvmproto.pong_payload(1, 2)), (1, 2))

    def test_cmd(self):
        self.assertEqual(kvmproto.decode_cmd(kvmproto.cmd_payload(3, 0)), (3, 0))


if __name__ == "__main__":
    unittest.main()
