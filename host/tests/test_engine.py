import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import engine  # noqa: E402
import hidmap  # noqa: E402
import kvmproto  # noqa: E402
from kvmproto import KeyState, MouseState  # noqa: E402

TOKEN = 0x0123456789ABCDEF


class FakeSink:
    def __init__(self):
        self.events = []
        self.moves = []

    def key(self, vk, down, extended=False):
        self.events.append(("key", vk, down, extended))
        return True

    def move(self, dx, dy):
        self.moves.append((dx, dy))
        return True

    def button(self, index, down):
        self.events.append(("btn", index, down))
        return True

    def wheel(self, steps):
        self.events.append(("wheel", steps))
        return True

    def hwheel(self, steps):
        self.events.append(("hwheel", steps))
        return True


def frame(msg_type, payload, seq=1, dev_ms=0):
    return kvmproto.decode(kvmproto.encode(msg_type, seq, TOKEN, payload, dev_ms=dev_ms))


class EngineTest(unittest.TestCase):
    def setUp(self):
        self.sink = FakeSink()
        self.eng = engine.InputEngine(self.sink, engine.EngineConfig(repeat=False))
        self.t = 100.0

    def feed(self, msg_type, payload):
        self.eng.handle_frame(frame(msg_type, payload), now=self.t)

    def feed_key(self, usages=(), mods=0, page=7):
        self.feed(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(page, mods, tuple(usages))))

    def test_press_release(self):
        self.feed_key([0x04])
        self.assertEqual(self.sink.events, [("key", 0x41, True, False)])
        self.feed_key([])
        self.assertEqual(self.sink.events[-1], ("key", 0x41, False, False))

    def test_modifier_first_and_last(self):
        self.feed_key([0x06], mods=0x01)  # Ctrl + C
        self.assertEqual(self.sink.events[0], ("key", hidmap.VK_LCONTROL, True, False))
        self.assertEqual(self.sink.events[1], ("key", 0x43, True, False))
        self.sink.events.clear()
        self.feed_key([])
        # 先释放字符键，再释放修饰键
        self.assertEqual(self.sink.events[0], ("key", 0x43, False, False))
        self.assertEqual(self.sink.events[1], ("key", hidmap.VK_LCONTROL, False, False))

    def test_no_duplicate_events(self):
        self.feed_key([0x04])
        self.feed_key([0x04])
        self.assertEqual(len(self.sink.events), 1)

    def test_consumer(self):
        self.feed_key([0x00E9], page=kvmproto.PAGE_CONSUMER)
        self.assertEqual(self.sink.events, [
            ("key", hidmap.VK_VOLUME_UP, True, False),
            ("key", hidmap.VK_VOLUME_UP, False, False),
        ])
        self.feed_key([0x00E9], page=kvmproto.PAGE_CONSUMER)
        self.assertEqual(len(self.sink.events), 2)  # 同一按键不重复触发

    def test_mouse_buttons(self):
        self.feed(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(buttons=1)))
        self.feed(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(buttons=0)))
        self.assertEqual(self.sink.events, [("btn", 0, True), ("btn", 0, False)])

    def test_mouse_scale(self):
        eng = engine.InputEngine(self.sink, engine.EngineConfig(mouse_scale=2.0))
        eng.handle_frame(frame(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(dx=3, dy=-4))), now=self.t)
        self.assertEqual(self.sink.moves, [(6.0, -8.0)])

    def test_wheel(self):
        self.feed(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(wheel=2, pan=-1)))
        self.assertEqual(self.sink.events, [("wheel", 2), ("hwheel", -1)])

    def test_repeat(self):
        eng = engine.InputEngine(self.sink, engine.EngineConfig(repeat=True, repeat_delay=0.1, repeat_interval=0.05))
        eng.handle_frame(frame(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(7, 0, (0x04,)))), now=10.0)
        self.assertEqual(len(self.sink.events), 1)
        eng.tick(now=10.05)
        self.assertEqual(len(self.sink.events), 1)
        eng.tick(now=10.2)
        self.assertEqual(len(self.sink.events), 2)
        eng.tick(now=10.22)
        self.assertEqual(len(self.sink.events), 2)
        eng.tick(now=10.3)
        self.assertEqual(len(self.sink.events), 3)

    def test_watchdog_releases(self):
        eng = engine.InputEngine(self.sink, engine.EngineConfig(watchdog_timeout=1.5, repeat=False))
        eng.handle_frame(frame(kvmproto.MSG_KEY, kvmproto.key_payload(KeyState(7, 0, (0x04,)))), now=0.0)
        eng.handle_frame(frame(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(buttons=1))), now=0.0)
        eng.tick(now=0.5)
        self.assertEqual(len(self.sink.events), 2)  # 键按下 + 鼠标键按下
        eng.tick(now=2.0)
        downs = [e for e in self.sink.events if e[2] is False]
        self.assertEqual(len(downs), 2)

    def test_release_message(self):
        self.feed_key([0x04])
        self.feed(kvmproto.MSG_RELEASE, b"")
        self.assertEqual(self.sink.events[-1], ("key", 0x41, False, False))

    def test_stats(self):
        self.feed_key([0x04])
        self.feed(kvmproto.MSG_MOUSE, kvmproto.mouse_payload(MouseState(dx=1)))
        self.assertEqual(self.eng.stats.frames, 2)
        self.assertEqual(self.eng.stats.keys, 1)
        self.assertEqual(self.eng.stats.mouse, 1)


if __name__ == "__main__":
    unittest.main()
