"""Windows 输入注入（SendInput）。

坐标系说明：
- 相对模式：MOUSEEVENTF_MOVE，受 Windows "提高指针精确度" 加速影响。
- 跟踪模式（默认）：先 GetCursorPos，再按增量计算目标点，用
  MOUSEEVENTF_ABSOLUTE|MOUSEEVENTF_VIRTUALDESK 定位，1:1 精确、不受加速影响，
  也支持多显示器拼接成的虚拟桌面。
"""

from __future__ import annotations

import ctypes
import sys
from ctypes import wintypes

if sys.platform != "win32":  # pragma: no cover - 仅 Windows 可用
    raise ImportError("wininject 只能在 Windows 上使用")

user32 = ctypes.WinDLL("user32", use_last_error=True)

ULONG_PTR = ctypes.c_size_t
LONG = wintypes.LONG
DWORD = wintypes.DWORD
WORD = wintypes.WORD

INPUT_MOUSE = 0
INPUT_KEYBOARD = 1

KEYEVENTF_EXTENDEDKEY = 0x0001
KEYEVENTF_KEYUP = 0x0002
KEYEVENTF_UNICODE = 0x0004
KEYEVENTF_SCANCODE = 0x0008

MOUSEEVENTF_MOVE = 0x0001
MOUSEEVENTF_LEFTDOWN = 0x0002
MOUSEEVENTF_LEFTUP = 0x0004
MOUSEEVENTF_RIGHTDOWN = 0x0008
MOUSEEVENTF_RIGHTUP = 0x0010
MOUSEEVENTF_MIDDLEDOWN = 0x0020
MOUSEEVENTF_MIDDLEUP = 0x0040
MOUSEEVENTF_XDOWN = 0x0080
MOUSEEVENTF_XUP = 0x0100
MOUSEEVENTF_WHEEL = 0x0800
MOUSEEVENTF_HWHEEL = 0x1000
MOUSEEVENTF_ABSOLUTE = 0x8000
MOUSEEVENTF_VIRTUALDESK = 0x4000

XBUTTON1 = 0x0001
XBUTTON2 = 0x0002

SM_XVIRTUALSCREEN = 76
SM_YVIRTUALSCREEN = 77
SM_CXVIRTUALSCREEN = 78
SM_CYVIRTUALSCREEN = 79


class KEYBDINPUT(ctypes.Structure):
    _fields_ = [
        ("wVk", WORD),
        ("wScan", WORD),
        ("dwFlags", DWORD),
        ("time", DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class MOUSEINPUT(ctypes.Structure):
    _fields_ = [
        ("dx", LONG),
        ("dy", LONG),
        ("mouseData", DWORD),
        ("dwFlags", DWORD),
        ("time", DWORD),
        ("dwExtraInfo", ULONG_PTR),
    ]


class HARDWAREINPUT(ctypes.Structure):
    _fields_ = [
        ("uMsg", DWORD),
        ("wParamL", WORD),
        ("wParamH", WORD),
    ]


class _INPUTUNION(ctypes.Union):
    _fields_ = [
        ("ki", KEYBDINPUT),
        ("mi", MOUSEINPUT),
        ("hi", HARDWAREINPUT),
    ]


class INPUT(ctypes.Structure):
    _anonymous_ = ("u",)
    _fields_ = [
        ("type", DWORD),
        ("u", _INPUTUNION),
    ]


user32.SendInput.argtypes = (wintypes.UINT, ctypes.POINTER(INPUT), ctypes.c_int)
user32.SendInput.restype = wintypes.UINT
user32.GetSystemMetrics.argtypes = (ctypes.c_int,)
user32.GetSystemMetrics.restype = ctypes.c_int
user32.GetCursorPos.argtypes = (ctypes.POINTER(wintypes.POINT),)
user32.GetCursorPos.restype = wintypes.BOOL


def enable_dpi_awareness() -> None:
    """让 SendInput 的绝对坐标使用物理像素，多屏 DPI 不同也不会偏。"""
    try:
        # DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = -4
        if user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4)):
            return
    except AttributeError:
        pass
    try:
        shcore = ctypes.WinDLL("shcore")
        shcore.SetProcessDpiAwareness(2)  # PROCESS_PER_MONITOR_DPI_AWARE
        return
    except (OSError, AttributeError):
        pass
    try:
        user32.SetProcessDPIAware()
    except AttributeError:
        pass


class WindowsInjector:
    """把抽象输入事件翻译成 SendInput。"""

    def __init__(self, mouse_mode: str = "track", dpi_aware: bool = True) -> None:
        if mouse_mode not in ("track", "relative"):
            raise ValueError("mouse_mode 只能是 'track' 或 'relative'")
        self.mouse_mode = mouse_mode
        self._frac_x = 0.0
        self._frac_y = 0.0
        if dpi_aware:
            enable_dpi_awareness()

    # -- 底层 ------------------------------------------------------------
    def _send(self, inputs: list[INPUT]) -> bool:
        if not inputs:
            return True
        arr = (INPUT * len(inputs))(*inputs)
        sent = user32.SendInput(len(inputs), arr, ctypes.sizeof(INPUT))
        return sent == len(inputs)

    @staticmethod
    def _key_input(vk: int, flags: int) -> INPUT:
        inp = INPUT()
        inp.type = INPUT_KEYBOARD
        inp.ki = KEYBDINPUT(wVk=vk, wScan=0, dwFlags=flags, time=0, dwExtraInfo=0)
        return inp

    @staticmethod
    def _mouse_input(dx: int, dy: int, data: int, flags: int) -> INPUT:
        inp = INPUT()
        inp.type = INPUT_MOUSE
        inp.mi = MOUSEINPUT(dx=dx, dy=dy, mouseData=data & 0xFFFFFFFF, dwFlags=flags, time=0, dwExtraInfo=0)
        return inp

    # -- 键盘 ------------------------------------------------------------
    def key(self, vk: int, down: bool, extended: bool = False) -> bool:
        flags = 0
        if extended:
            flags |= KEYEVENTF_EXTENDEDKEY
        if not down:
            flags |= KEYEVENTF_KEYUP
        return self._send([self._key_input(vk, flags)])

    def key_multiple(self, events: list[tuple[int, bool, bool]]) -> bool:
        payload = []
        for vk, down, extended in events:
            flags = KEYEVENTF_EXTENDEDKEY if extended else 0
            if not down:
                flags |= KEYEVENTF_KEYUP
            payload.append(self._key_input(vk, flags))
        return self._send(payload)

    # -- 鼠标 ------------------------------------------------------------
    def _virtual_desktop(self) -> tuple[int, int, int, int]:
        left = user32.GetSystemMetrics(SM_XVIRTUALSCREEN)
        top = user32.GetSystemMetrics(SM_YVIRTUALSCREEN)
        width = user32.GetSystemMetrics(SM_CXVIRTUALSCREEN)
        height = user32.GetSystemMetrics(SM_CYVIRTUALSCREEN)
        if width <= 0 or height <= 0:
            width, height = 1920, 1080
        return left, top, width, height

    @staticmethod
    def cursor_pos() -> tuple[int, int]:
        pt = wintypes.POINT()
        if not user32.GetCursorPos(ctypes.byref(pt)):
            return 0, 0
        return pt.x, pt.y

    def _move_absolute(self, x: int, y: int) -> bool:
        left, top, width, height = self._virtual_desktop()
        # 规范化到 0..65535（含端点）
        nx = 0 if width <= 1 else round((x - left) * 65535 / (width - 1))
        ny = 0 if height <= 1 else round((y - top) * 65535 / (height - 1))
        nx = max(0, min(65535, nx))
        ny = max(0, min(65535, ny))
        return self._send([
            self._mouse_input(nx, ny, 0, MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK)
        ])

    def move(self, dx: float, dy: float) -> bool:
        """按像素增量移动。dx/dy 可以是浮点，内部保留亚像素余量。"""
        if self.mouse_mode == "relative":
            self._frac_x += dx
            self._frac_y += dy
            ix, iy = int(self._frac_x), int(self._frac_y)
            self._frac_x -= ix
            self._frac_y -= iy
            if ix == 0 and iy == 0:
                return True
            return self._send([self._mouse_input(ix, iy, 0, MOUSEEVENTF_MOVE)])

        self._frac_x += dx
        self._frac_y += dy
        ix, iy = int(self._frac_x), int(self._frac_y)
        self._frac_x -= ix
        self._frac_y -= iy
        if ix == 0 and iy == 0:
            return True
        cx, cy = self.cursor_pos()
        left, top, width, height = self._virtual_desktop()
        tx = max(left, min(left + width - 1, cx + ix))
        ty = max(top, min(top + height - 1, cy + iy))
        return self._move_absolute(tx, ty)

    def move_absolute(self, x: int, y: int) -> bool:
        return self._move_absolute(x, y)

    # 按键索引：0=左 1=右 2=中 3=侧键1 4=侧键2
    _BUTTON_FLAGS = {
        0: (MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP, 0),
        1: (MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP, 0),
        2: (MOUSEEVENTF_MIDDLEDOWN, MOUSEEVENTF_MIDDLEUP, 0),
        3: (MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON1),
        4: (MOUSEEVENTF_XDOWN, MOUSEEVENTF_XUP, XBUTTON2),
    }

    def button(self, index: int, down: bool) -> bool:
        flags = self._BUTTON_FLAGS.get(index)
        if flags is None:
            return False
        down_flag, up_flag, data = flags
        return self._send([self._mouse_input(0, 0, data, down_flag if down else up_flag)])

    def wheel(self, steps: int) -> bool:
        if steps == 0:
            return True
        return self._send([self._mouse_input(0, 0, (steps * 120) & 0xFFFFFFFF, MOUSEEVENTF_WHEEL)])

    def hwheel(self, steps: int) -> bool:
        if steps == 0:
            return True
        return self._send([self._mouse_input(0, 0, (steps * 120) & 0xFFFFFFFF, MOUSEEVENTF_HWHEEL)])


class SecureDesktopInjector(WindowsInjector):
    """以 SYSTEM 权限运行在用户会话时，跟随当前输入桌面（含锁屏 Winlogon）。

    普通用户进程只能注入 Default 桌面；服务模式必须切到当前输入桌面，
    否则锁屏/UAC 时 SendInput 会被系统丢弃。
    """

    DESKTOP_ALL = 0x10000000  # GENERIC_ALL
    DESKTOP_SWITCH = 0x0100

    def __init__(self, *args, **kwargs) -> None:
        super().__init__(*args, **kwargs)
        self._desk = None

    def _ensure_input_desktop(self) -> None:
        h = user32.OpenInputDesktop(0, False, self.DESKTOP_ALL)
        if not h:
            h = user32.OpenInputDesktop(0, False, self.DESKTOP_SWITCH)
        if not h:
            return
        if h == self._desk:
            return
        if user32.SetThreadDesktop(h):
            if self._desk:
                user32.CloseDesktop(self._desk)
            self._desk = h
        else:
            user32.CloseDesktop(h)

    def _send(self, inputs):
        try:
            self._ensure_input_desktop()
        except Exception:  # noqa: BLE001
            pass
        return super()._send(inputs)
