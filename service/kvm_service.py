"""KVM-Wire SYSTEM 服务：在用户会话里以 SYSTEM 权限运行注入助手。

普通用户进程无法向锁屏/UAC 的 Secure Desktop 注入 SendInput；
服务以 SYSTEM 权限在活动控制台会话创建助手进程，助手跟随当前输入桌面
（Default / Winlogon）后再注入。

用法：
  KVM-Wire-Service.exe install     # 管理员
  KVM-Wire-Service.exe start
  KVM-Wire-Service.exe stop
  KVM-Wire-Service.exe remove
  KVM-Wire-Service.exe --helper <config.json>   # 内部使用
"""

from __future__ import annotations

import json
import os
import sys
import time
from pathlib import Path

import servicemanager
import win32api
import win32con
import win32event
import win32process
import win32security
import win32service
import win32serviceutil
import win32ts

SERVICE_NAME = "KVMWireSecure"
SERVICE_DISPLAY = "KVM-Wire Secure Input"
CONFIG_PATH = Path(os.environ.get("ProgramData", r"C:\ProgramData")) / "KVM-Wire" / "service.json"
LOG_PATH = CONFIG_PATH.parent / "service.log"


def log(msg: str) -> None:
    try:
        CONFIG_PATH.parent.mkdir(parents=True, exist_ok=True)
        with open(LOG_PATH, "a", encoding="utf-8") as fh:
            fh.write(f"{time.strftime('%Y-%m-%d %H:%M:%S')} {msg}\n")
    except Exception:  # noqa: BLE001
        pass


def run_helper(config_path: str) -> int:
    cfg = json.loads(Path(config_path).read_text(encoding="utf-8-sig"))
    os.environ["KVM_SECURE_DESKTOP"] = "1"
    sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "host"))
    from kvm_agent import AgentConfig, KvmAgent

    agent_cfg = AgentConfig(
        relay_host=cfg.get("relay_host"),
        relay_port=int(cfg.get("relay_port", 8790)),
        relay_ca=cfg.get("ca"),
        relay_key=cfg.get("relay_key"),
        relay_device=cfg.get("device_id") or None,
        e2e_key=cfg.get("pair_code") or None,
        dry_run=bool(cfg.get("dry_run", False)),
    )
    log("helper 启动")
    KvmAgent(agent_cfg).run()
    return 0


class KvmService(win32serviceutil.ServiceFramework):
    _svc_name_ = SERVICE_NAME
    _svc_display_name_ = SERVICE_DISPLAY
    _svc_description_ = "KVM-Wire 安全桌面输入服务（SYSTEM）"

    def __init__(self, args):
        super().__init__(args)
        self._stop = False
        self._child = None
        self._child_session = -1

    def SvcStop(self):
        self.ReportServiceStatus(win32service.SERVICE_STOP_PENDING)
        self._stop = True
        self._kill_child()

    def _kill_child(self):
        if self._child is None:
            return
        try:
            if win32event.WaitForSingleObject(self._child, 0) != win32event.WAIT_OBJECT_0:
                win32api.TerminateProcess(self._child, 1)
        except Exception as exc:  # noqa: BLE001
            log(f"终止 helper 失败: {exc}")
        self._child = None

    def _launch(self, session: int):
        self._kill_child()
        exe = sys.executable
        cmd = f'"{exe}" --helper "{CONFIG_PATH}"'
        htok = win32security.OpenProcessToken(win32api.GetCurrentProcess(), win32con.TOKEN_ALL_ACCESS)
        try:
            win32security.AdjustTokenPrivileges(
                htok,
                False,
                [(win32security.LookupPrivilegeValue(None, win32security.SE_TCB_NAME), True)],
            )
        except Exception:  # noqa: BLE001
            pass
        token = win32security.DuplicateTokenEx(
            htok,
            win32con.TOKEN_ALL_ACCESS,
            None,
            win32security.SecurityImpersonation,
            win32con.TokenPrimary,
        )
        win32security.SetTokenInformation(token, win32con.TokenSessionId, session)
        si = win32process.STARTUPINFO()
        si.lpDesktop = "winsta0\\default"
        proc, thread, pid, _tid = win32process.CreateProcessAsUser(
            token, exe, cmd, None, None, False, 0, None, None, si
        )
        win32api.CloseHandle(thread)
        win32api.CloseHandle(token)
        win32api.CloseHandle(htok)
        self._child = proc
        self._child_session = session
        log(f"helper 已启动 session={session} pid={pid}")

    def SvcDoRun(self):
        servicemanager.LogInfoMsg("KVM-Wire 服务启动")
        log("服务启动")
        try:
            while not self._stop:
                session = win32ts.WTSGetActiveConsoleSessionId()
                if session == 0xFFFFFFFF:
                    time.sleep(2)
                    continue
                alive = False
                if self._child is not None:
                    alive = (
                        win32event.WaitForSingleObject(self._child, 0) != win32event.WAIT_OBJECT_0
                    )
                if not alive or session != self._child_session:
                    self._launch(session)
                time.sleep(2)
        except Exception as exc:  # noqa: BLE001
            log(f"服务异常: {exc!r}")
            servicemanager.LogErrorMsg(f"KVM-Wire 服务异常: {exc!r}")
        finally:
            self._kill_child()
            log("服务停止")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "--helper":
        if len(sys.argv) < 3:
            sys.exit("用法: --helper <config.json>")
        raise SystemExit(run_helper(sys.argv[2]))

    if len(sys.argv) > 1 and sys.argv[1] in (
        "install", "remove", "start", "stop", "restart", "update", "debug",
    ):
        win32serviceutil.HandleCommandLine(KvmService)
    else:
        servicemanager.Initialize()
        servicemanager.PrepareToHostSingle(KvmService)
        servicemanager.StartServiceCtrlDispatcher()
