"""HID usage -> Windows Virtual-Key 映射。

只做映射，不做注入（注入在 wininject.py）。
"""

from __future__ import annotations

# --- Virtual-Key codes -----------------------------------------------------
VK_BACK = 0x08
VK_TAB = 0x09
VK_RETURN = 0x0D
VK_PAUSE = 0x13
VK_CAPITAL = 0x14
VK_ESCAPE = 0x1B
VK_SPACE = 0x20
VK_PRIOR = 0x21
VK_NEXT = 0x22
VK_END = 0x23
VK_HOME = 0x24
VK_LEFT = 0x25
VK_UP = 0x26
VK_RIGHT = 0x27
VK_DOWN = 0x28
VK_SNAPSHOT = 0x2C
VK_INSERT = 0x2D
VK_DELETE = 0x2E
VK_LWIN = 0x5B
VK_RWIN = 0x5C
VK_APPS = 0x5D
VK_NUMPAD0 = 0x60
VK_NUMPAD1 = 0x61
VK_NUMPAD2 = 0x62
VK_NUMPAD3 = 0x63
VK_NUMPAD4 = 0x64
VK_NUMPAD5 = 0x65
VK_NUMPAD6 = 0x66
VK_NUMPAD7 = 0x67
VK_NUMPAD8 = 0x68
VK_NUMPAD9 = 0x69
VK_MULTIPLY = 0x6A
VK_ADD = 0x6B
VK_SEPARATOR = 0x6C
VK_SUBTRACT = 0x6D
VK_DECIMAL = 0x6E
VK_DIVIDE = 0x6F
VK_F1 = 0x70
VK_F24 = 0x87
VK_NUMLOCK = 0x90
VK_SCROLL = 0x91
VK_LSHIFT = 0xA0
VK_RSHIFT = 0xA1
VK_LCONTROL = 0xA2
VK_RCONTROL = 0xA3
VK_LMENU = 0xA4
VK_RMENU = 0xA5
VK_BROWSER_BACK = 0xA6
VK_BROWSER_FORWARD = 0xA7
VK_BROWSER_REFRESH = 0xA8
VK_BROWSER_STOP = 0xA9
VK_BROWSER_SEARCH = 0xAA
VK_BROWSER_FAVORITES = 0xAB
VK_BROWSER_HOME = 0xAC
VK_VOLUME_MUTE = 0xAD
VK_VOLUME_DOWN = 0xAE
VK_VOLUME_UP = 0xAF
VK_MEDIA_NEXT_TRACK = 0xB0
VK_MEDIA_PREV_TRACK = 0xB1
VK_MEDIA_STOP = 0xB2
VK_MEDIA_PLAY_PAUSE = 0xB3
VK_LAUNCH_MAIL = 0xB4
VK_LAUNCH_MEDIA_SELECT = 0xB5
VK_LAUNCH_APP1 = 0xB6
VK_LAUNCH_APP2 = 0xB7
VK_OEM_1 = 0xBA        # ;:
VK_OEM_PLUS = 0xBB     # =+
VK_OEM_COMMA = 0xBC    # ,<
VK_OEM_MINUS = 0xBD    # -_
VK_OEM_PERIOD = 0xBE   # .>
VK_OEM_2 = 0xBF        # /?
VK_OEM_3 = 0xC0        # `~
VK_OEM_4 = 0xDB        # [{
VK_OEM_5 = 0xDC        # \|
VK_OEM_6 = 0xDD        # ]}
VK_OEM_7 = 0xDE        # '"
VK_OEM_102 = 0xE2      # 非美键盘的 <> 键

# usage -> (vk, extended)
KEYBOARD_USAGE_TO_VK: dict[int, tuple[int, bool]] = {}

for _i in range(26):  # 0x04..0x1D -> A..Z
    KEYBOARD_USAGE_TO_VK[0x04 + _i] = (0x41 + _i, False)

for _i, _vk in enumerate([0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x30]):
    KEYBOARD_USAGE_TO_VK[0x1E + _i] = (_vk, False)  # 1..9, 0

KEYBOARD_USAGE_TO_VK.update({
    0x28: (VK_RETURN, False),
    0x29: (VK_ESCAPE, False),
    0x2A: (VK_BACK, False),
    0x2B: (VK_TAB, False),
    0x2C: (VK_SPACE, False),
    0x2D: (VK_OEM_MINUS, False),
    0x2E: (VK_OEM_PLUS, False),
    0x2F: (VK_OEM_4, False),
    0x30: (VK_OEM_6, False),
    0x31: (VK_OEM_5, False),
    0x32: (VK_OEM_102, False),
    0x33: (VK_OEM_1, False),
    0x34: (VK_OEM_7, False),
    0x35: (VK_OEM_3, False),
    0x36: (VK_OEM_COMMA, False),
    0x37: (VK_OEM_PERIOD, False),
    0x38: (VK_OEM_2, False),
    0x39: (VK_CAPITAL, False),
    0x46: (VK_SNAPSHOT, True),
    0x47: (VK_SCROLL, False),
    0x48: (VK_PAUSE, False),
    0x49: (VK_INSERT, True),
    0x4A: (VK_HOME, True),
    0x4B: (VK_PRIOR, True),
    0x4C: (VK_DELETE, True),
    0x4D: (VK_END, True),
    0x4E: (VK_NEXT, True),
    0x4F: (VK_RIGHT, True),
    0x50: (VK_LEFT, True),
    0x51: (VK_DOWN, True),
    0x52: (VK_UP, True),
    0x53: (VK_NUMLOCK, True),
    0x54: (VK_DIVIDE, True),
    0x55: (VK_MULTIPLY, False),
    0x56: (VK_SUBTRACT, False),
    0x57: (VK_ADD, False),
    0x58: (VK_RETURN, True),  # 小键盘 Enter
    0x59: (VK_NUMPAD1, False),
    0x5A: (VK_NUMPAD2, False),
    0x5B: (VK_NUMPAD3, False),
    0x5C: (VK_NUMPAD4, False),
    0x5D: (VK_NUMPAD5, False),
    0x5E: (VK_NUMPAD6, False),
    0x5F: (VK_NUMPAD7, False),
    0x60: (VK_NUMPAD8, False),
    0x61: (VK_NUMPAD9, False),
    0x62: (VK_NUMPAD0, False),
    0x63: (VK_DECIMAL, False),
    0x64: (VK_OEM_102, False),
    0x67: (VK_APPS, True),
})

for _i in range(12):  # F1..F12 -> 0x3A..0x45
    KEYBOARD_USAGE_TO_VK[0x3A + _i] = (VK_F1 + _i, False)
for _i in range(12):  # F13..F24 -> 0x68..0x73
    KEYBOARD_USAGE_TO_VK[0x68 + _i] = (VK_F1 + 12 + _i, False)

# 修饰键 usage 0xE0..0xE7
MODIFIER_USAGE_TO_VK = {
    0xE0: (VK_LCONTROL, False),
    0xE1: (VK_LSHIFT, False),
    0xE2: (VK_LMENU, False),
    0xE3: (VK_LWIN, True),
    0xE4: (VK_RCONTROL, True),
    0xE5: (VK_RSHIFT, False),
    0xE6: (VK_RMENU, True),
    0xE7: (VK_RWIN, True),
}
KEYBOARD_USAGE_TO_VK.update(MODIFIER_USAGE_TO_VK)

# 消费类按键（page 0x0C）-> vk
CONSUMER_USAGE_TO_VK = {
    0x00B0: VK_LAUNCH_MEDIA_SELECT,
    0x00B3: VK_MEDIA_PLAY_PAUSE,
    0x00B4: VK_MEDIA_STOP,
    0x00B5: VK_MEDIA_NEXT_TRACK,
    0x00B6: VK_MEDIA_PREV_TRACK,
    0x00CD: VK_LAUNCH_MEDIA_SELECT,
    0x00E2: VK_VOLUME_MUTE,
    0x00E9: VK_VOLUME_UP,
    0x00EA: VK_VOLUME_DOWN,
    0x0183: VK_LAUNCH_MEDIA_SELECT,
    0x0192: VK_LAUNCH_APP2,       # 计算器
    0x0194: VK_LAUNCH_MAIL,
    0x0196: VK_BROWSER_HOME,
    0x0221: VK_BROWSER_SEARCH,
    0x0223: VK_BROWSER_BACK,
    0x0224: VK_BROWSER_FORWARD,
    0x0225: VK_BROWSER_STOP,
    0x0226: VK_BROWSER_REFRESH,
    0x0227: VK_BROWSER_FAVORITES,
}

MODIFIER_USAGES = (0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7)

_USAGE_NAMES = {
    0x28: "Enter", 0x29: "Esc", 0x2A: "Backspace", 0x2B: "Tab", 0x2C: "Space",
    0x39: "CapsLock", 0x46: "PrintScreen", 0x47: "ScrollLock", 0x48: "Pause",
    0x49: "Insert", 0x4A: "Home", 0x4B: "PageUp", 0x4C: "Delete", 0x4D: "End",
    0x4E: "PageDown", 0x4F: "Right", 0x50: "Left", 0x51: "Down", 0x52: "Up",
    0x53: "NumLock", 0x54: "KP/", 0x55: "KP*", 0x56: "KP-", 0x57: "KP+",
    0x58: "KPEnter", 0x63: "KP.", 0x67: "Menu",
    0xE0: "LCtrl", 0xE1: "LShift", 0xE2: "LAlt", 0xE3: "LWin",
    0xE4: "RCtrl", 0xE5: "RShift", 0xE6: "RAlt", 0xE7: "RWin",
}


def usage_name(usage: int) -> str:
    if usage in _USAGE_NAMES:
        return _USAGE_NAMES[usage]
    if 0x04 <= usage <= 0x1D:
        return chr(0x41 + usage - 0x04)
    if 0x1E <= usage <= 0x27:
        return "1234567890"[usage - 0x1E]
    if 0x3A <= usage <= 0x45:
        return f"F{usage - 0x3A + 1}"
    if 0x68 <= usage <= 0x73:
        return f"F{usage - 0x68 + 13}"
    if 0x59 <= usage <= 0x62:
        return f"KP{usage - 0x58}"
    return f"U+{usage:04X}"


def consumer_name(usage: int) -> str:
    names = {
        0x00B3: "PlayPause", 0x00B4: "Stop", 0x00B5: "NextTrack", 0x00B6: "PrevTrack",
        0x00E2: "Mute", 0x00E9: "VolumeUp", 0x00EA: "VolumeDown",
        0x0192: "Calculator", 0x0194: "Mail", 0x0196: "BrowserHome",
        0x0221: "BrowserSearch", 0x0223: "BrowserBack", 0x0224: "BrowserForward",
        0x0225: "BrowserStop", 0x0226: "BrowserRefresh", 0x0227: "BrowserFavorites",
    }
    return names.get(usage, f"C+{usage:04X}")


def key_usage_to_vk(usage: int) -> tuple[int, bool] | None:
    return KEYBOARD_USAGE_TO_VK.get(usage)


def consumer_usage_to_vk(usage: int) -> int | None:
    return CONSUMER_USAGE_TO_VK.get(usage)


def is_modifier_usage(usage: int) -> bool:
    return usage in MODIFIER_USAGE_TO_VK
