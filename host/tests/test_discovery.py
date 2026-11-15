import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import kvmproto  # noqa: E402
from discovery import LanDiscovery  # noqa: E402


class DiscoveryTest(unittest.TestCase):
    def setUp(self):
        self.disc = LanDiscovery(8766)

    def tearDown(self):
        self.disc.close()

    def test_broadcast_targets(self):
        targets = self.disc.broadcast_targets()
        self.assertIn("255.255.255.255", targets)
        self.assertIn("127.0.0.1", targets)
        self.assertTrue(any(t.endswith(".255") for t in targets if t != "255.255.255.255"))

    def test_record_hello(self):
        payload = kvmproto.hello_payload(mac=b"\xaa\xbb\xcc\xdd\xee\xff", name="kvm-test")
        frame = kvmproto.encode(kvmproto.MSG_HELLO, 3, 0x1122334455667788, payload)
        self.disc._record(frame, ("192.168.1.9", 8766))
        devices = self.disc.devices()
        self.assertEqual(len(devices), 1)
        d = devices[0]
        self.assertEqual(d.name, "kvm-test")
        self.assertEqual(d.mac, "aa:bb:cc:dd:ee:ff")
        self.assertEqual(d.token_hex, "1122334455667788")
        self.assertEqual((d.addr, d.port), ("192.168.1.9", 8766))

        # 同一设备再次出现应去重（按 MAC）
        self.disc._record(frame, ("192.168.1.9", 8766))
        self.assertEqual(len(self.disc.devices()), 1)

    def test_ignores_other_messages(self):
        frame = kvmproto.encode(kvmproto.MSG_KEY, 1, 0, kvmproto.key_payload(kvmproto.KeyState(7, 0, (4,))))
        self.disc._record(frame, ("192.168.1.9", 8766))
        self.assertEqual(len(self.disc.devices()), 0)


if __name__ == "__main__":
    unittest.main()
