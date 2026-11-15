import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import hidmap  # noqa: E402


class HidMapTest(unittest.TestCase):
    def test_letters(self):
        self.assertEqual(hidmap.key_usage_to_vk(0x04), (0x41, False))  # A
        self.assertEqual(hidmap.key_usage_to_vk(0x1D), (0x5A, False))  # Z

    def test_digits(self):
        self.assertEqual(hidmap.key_usage_to_vk(0x1E)[0], 0x31)  # 1
        self.assertEqual(hidmap.key_usage_to_vk(0x27)[0], 0x30)  # 0

    def test_extended(self):
        self.assertEqual(hidmap.key_usage_to_vk(0x4F), (hidmap.VK_RIGHT, True))
        self.assertEqual(hidmap.key_usage_to_vk(0x28), (hidmap.VK_RETURN, False))

    def test_modifiers(self):
        self.assertEqual(hidmap.key_usage_to_vk(0xE0), (hidmap.VK_LCONTROL, False))
        self.assertEqual(hidmap.key_usage_to_vk(0xE4), (hidmap.VK_RCONTROL, True))

    def test_consumer(self):
        self.assertEqual(hidmap.consumer_usage_to_vk(0x00E9), hidmap.VK_VOLUME_UP)
        self.assertIsNone(hidmap.consumer_usage_to_vk(0x1234))

    def test_names(self):
        self.assertEqual(hidmap.usage_name(0x04), "A")
        self.assertEqual(hidmap.usage_name(0x28), "Enter")
        self.assertEqual(hidmap.usage_name(0x3A), "F1")


if __name__ == "__main__":
    unittest.main()
