#!/usr/bin/env python3
"""ESP32-S3 KVM 图形控制台。

启动后自动扫描局域网内的设备，选中后点“连接”，确认后才开始注入。
没有托盘/服务，关闭窗口即停止注入并释放所有按键。
"""

from __future__ import annotations

import sys
import threading
import time
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

import pystray
import sv_ttk
from PIL import Image

import kvmproto
from discovery import DiscoveredDevice, LanDiscovery
from kvm_agent import AgentConfig, KvmAgent


import json


def gui_config_path() -> Path:
    if getattr(sys, "frozen", False):
        return Path(sys.executable).resolve().parent / "kvm-wire.json"
    return Path(__file__).resolve().parent.parent / "kvm-wire.json"


def resource_path(rel: str) -> Path:
    base = Path(getattr(sys, "_MEIPASS", Path(__file__).resolve().parent.parent))
    for cand in (base / rel, Path(__file__).resolve().parent.parent / rel):
        if cand.exists():
            return cand
    return base / rel


def default_ca_path() -> str:
    """打包后优先用 exe 同目录的 ca.crt，源码运行时用仓库 server/ca.crt。"""
    if getattr(sys, "frozen", False):
        base = Path(sys.executable).resolve().parent
        candidates = [base / "ca.crt", base / "server" / "ca.crt"]
    else:
        base = Path(__file__).resolve().parent.parent
        candidates = [base / "server" / "ca.crt", base / "ca.crt"]
    for path in candidates:
        if path.exists():
            return str(path)
    return str(candidates[0])


class KvmApp:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("KVM-Wire 控制台")
        self.root.geometry("880x600")
        self.root.minsize(760, 520)

        self.devices: list[DiscoveredDevice] = []
        self.agent: KvmAgent | None = None
        self.agent_thread: threading.Thread | None = None
        self.discovery = LanDiscovery(kvmproto.DEFAULT_DEVICE_PORT)
        self.scanning = False
        self.last_rtt: float | None = None

        self._build_ui()
        self._apply_config_file()
        self._quitting = False
        self.tray = None
        self._setup_tray()
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)
        self.root.after(200, self.scan_devices)
        self.root.after(400, self.update_status)

    # ------------------------------------------------------------------
    # UI
    # ------------------------------------------------------------------
    def _build_ui(self) -> None:
        style = ttk.Style()
        try:
            style.configure("Treeview", rowheight=26)
        except tk.TclError:
            pass

        outer = ttk.Frame(self.root, padding=12)
        outer.pack(fill="both", expand=True)

        header = ttk.Frame(outer)
        header.pack(fill="x")
        ttk.Label(header, text="KVM-Wire", font=("Segoe UI", 20, "bold")).pack(side="left")
        ttk.Label(header, text="蓝牙键鼠 · 网络 KVM", foreground="#666").pack(side="left", padx=(10, 0))
        self.scan_state = ttk.Label(header, text="正在扫描局域网…", foreground="#666")
        self.scan_state.pack(side="right")

        # ---- 设备列表 ----
        box = ttk.LabelFrame(outer, text="局域网设备（点击选择，双击或点“连接”）", padding=8)
        box.pack(fill="both", expand=True, pady=(10, 8))

        columns = ("name", "ip", "mac", "fw", "token")
        self.tree = ttk.Treeview(box, columns=columns, show="headings", height=6, selectmode="browse")
        widths = {"name": 190, "ip": 140, "mac": 140, "fw": 70, "token": 150}
        titles = {"name": "设备名", "ip": "IP:端口", "mac": "MAC", "fw": "固件", "token": "Token"}
        for col in columns:
            self.tree.heading(col, text=titles[col])
            self.tree.column(col, width=widths[col], anchor="w")
        self.tree.pack(side="left", fill="both", expand=True)
        self.tree.bind("<<TreeviewSelect>>", self.on_select)
        self.tree.bind("<Double-1>", lambda _e: self.connect_selected())

        bar = ttk.Frame(outer)
        bar.pack(fill="x")
        self.btn_scan = ttk.Button(bar, text="刷新设备", command=self.scan_devices)
        self.btn_scan.pack(side="left")
        self.btn_connect = ttk.Button(bar, text="连接选中设备", command=self.connect_selected, state="disabled")
        self.btn_connect.pack(side="left", padx=8)
        self.btn_disconnect = ttk.Button(bar, text="断开", command=self.disconnect, state="disabled")
        self.btn_disconnect.pack(side="left")
        ttk.Button(bar, text="释放全部按键", command=self.release_all).pack(side="right")

        # ---- 设置 ----
        settings = ttk.LabelFrame(outer, text="连接与注入设置", padding=8)
        settings.pack(fill="x", pady=(10, 8))

        row = ttk.Frame(settings)
        row.pack(fill="x", pady=2)
        ttk.Label(row, text="Token", width=8).pack(side="left")
        self.var_token = tk.StringVar(value="0123456789abcdef")
        ttk.Entry(row, textvariable=self.var_token, width=22).pack(side="left")
        ttk.Label(row, text="设备端口", width=9).pack(side="left", padx=(16, 0))
        self.var_device_port = tk.StringVar(value=str(kvmproto.DEFAULT_DEVICE_PORT))
        ttk.Entry(row, textvariable=self.var_device_port, width=7).pack(side="left")
        ttk.Label(row, text="本机监听端口", width=12).pack(side="left", padx=(16, 0))
        self.var_local_port = tk.StringVar(value=str(kvmproto.DEFAULT_AGENT_PORT))
        ttk.Entry(row, textvariable=self.var_local_port, width=7).pack(side="left")

        row2 = ttk.Frame(settings)
        row2.pack(fill="x", pady=2)
        ttk.Label(row2, text="鼠标模式", width=8).pack(side="left")
        self.var_mode = tk.StringVar(value="track")
        ttk.Combobox(row2, textvariable=self.var_mode, values=["track", "relative"], width=10,
                     state="readonly").pack(side="left")
        ttk.Label(row2, text="灵敏度", width=7).pack(side="left", padx=(16, 0))
        self.var_scale = tk.StringVar(value="1.0")
        ttk.Spinbox(row2, from_=0.2, to=4.0, increment=0.1, textvariable=self.var_scale, width=6).pack(side="left")
        ttk.Label(row2, text="滚轮倍率", width=9).pack(side="left", padx=(16, 0))
        self.var_wheel = tk.StringVar(value="1")
        ttk.Spinbox(row2, from_=1, to=5, textvariable=self.var_wheel, width=5).pack(side="left")
        self.var_repeat = tk.BooleanVar(value=True)
        ttk.Checkbutton(row2, text="长按重复", variable=self.var_repeat).pack(side="left", padx=(16, 0))

        # ---- 广域网中继 ----
        relay = ttk.LabelFrame(outer, text="广域网中继（远程时使用，可选）", padding=8)
        relay.pack(fill="x", pady=(10, 8))

        r1 = ttk.Frame(relay)
        r1.pack(fill="x", pady=2)
        ttk.Label(r1, text="中继地址", width=8).pack(side="left")
        self.var_relay_host = tk.StringVar()
        ttk.Entry(r1, textvariable=self.var_relay_host, width=24).pack(side="left")
        ttk.Label(r1, text="端口", width=5).pack(side="left", padx=(10, 0))
        self.var_relay_port = tk.StringVar(value="8790")
        ttk.Entry(r1, textvariable=self.var_relay_port, width=7).pack(side="left")
        ttk.Label(r1, text="设备 ID（可空）", width=13).pack(side="left", padx=(10, 0))
        self.var_relay_device = tk.StringVar()
        ttk.Entry(r1, textvariable=self.var_relay_device, width=16).pack(side="left")

        r2 = ttk.Frame(relay)
        r2.pack(fill="x", pady=2)
        ttk.Label(r2, text="客户端密钥", width=8).pack(side="left")
        self.var_relay_key = tk.StringVar()
        ttk.Entry(r2, textvariable=self.var_relay_key, width=30, show="*").pack(side="left")
        ttk.Label(r2, text="CA 证书", width=8).pack(side="left", padx=(10, 0))
        self.var_relay_ca = tk.StringVar(value=default_ca_path())
        ttk.Entry(r2, textvariable=self.var_relay_ca, width=28).pack(side="left")
        ttk.Button(r2, text="浏览", command=self.browse_ca).pack(side="left", padx=4)

        r3 = ttk.Frame(relay)
        r3.pack(fill="x", pady=2)
        ttk.Label(r3, text="设备配对码", width=8).pack(side="left")
        self.var_e2e = tk.StringVar()
        ttk.Entry(r3, textvariable=self.var_e2e, width=30, show="*").pack(side="left")
        ttk.Label(r3, text="填设备网页显示的配对码", foreground="#666").pack(side="left", padx=10)

        r4 = ttk.Frame(relay)
        r4.pack(fill="x", pady=(4, 0))
        self.btn_relay = ttk.Button(r4, text="连接中继", command=self.connect_relay)
        self.btn_relay.pack(side="left")
        ttk.Label(r4, text="留空设备 ID 时自动选择第一台在线设备", foreground="#666").pack(side="left", padx=10)

        # ---- 状态 ----
        status = ttk.LabelFrame(outer, text="状态", padding=8)
        status.pack(fill="x")
        self.status_vars = {}
        labels = [("state", "连接状态"), ("frames", "收包"), ("lost", "丢包"), ("rtt", "RTT"),
                  ("ble", "蓝牙设备")]
        for i, (key, text) in enumerate(labels):
            ttk.Label(status, text=text + "：", width=10).grid(row=0, column=i * 2, sticky="e")
            var = tk.StringVar(value="-")
            ttk.Label(status, textvariable=var, width=18).grid(row=0, column=i * 2 + 1, sticky="w")
            self.status_vars[key] = var

        self.hint = tk.StringVar(value="提示：先让设备连上同一个 WiFi，再点“刷新设备”。")
        ttk.Label(outer, textvariable=self.hint, foreground="#666").pack(fill="x", pady=(8, 0))

    # ------------------------------------------------------------------
    # 发现
    # ------------------------------------------------------------------
    def scan_devices(self) -> None:
        if self.agent is not None or self.scanning:
            return
        try:
            port = int(self.var_device_port.get())
        except ValueError:
            port = kvmproto.DEFAULT_DEVICE_PORT
        self.discovery.device_port = port
        self.scanning = True
        self.btn_scan.config(state="disabled")
        self.scan_state.config(text="正在扫描局域网…")

        def worker() -> None:
            devices = self.discovery.scan(2.5)
            self.root.after(0, lambda: self._apply_scan(devices))

        threading.Thread(target=worker, name="kvm-scan", daemon=True).start()

    def _apply_scan(self, devices: list[DiscoveredDevice]) -> None:
        self.scanning = False
        self.btn_scan.config(state="normal")
        self.devices = devices
        selected = self.selected_device()
        self.tree.delete(*self.tree.get_children())
        for i, d in enumerate(devices):
            self.tree.insert("", "end", iid=str(i), values=(
                d.name or "ESP32-S3 KVM",
                f"{d.addr}:{d.port}",
                d.mac,
                d.fw,
                d.token_hex,
            ))
        if selected is not None:
            for i, d in enumerate(devices):
                if d.key == selected.key:
                    self.tree.selection_set(str(i))
                    break
        self.scan_state.config(text=f"发现 {len(devices)} 台设备")
        if not devices:
            self.hint.set("没找到设备：确认设备上电、已连同一 WiFi、防火墙允许 UDP 8766。")
        else:
            self.hint.set("选中设备后点“连接”，会弹窗确认。")
        self._refresh_buttons()

    def selected_device(self) -> DiscoveredDevice | None:
        sel = self.tree.selection()
        if not sel:
            return None
        try:
            return self.devices[int(sel[0])]
        except (ValueError, IndexError):
            return None

    def on_select(self, _event=None) -> None:
        device = self.selected_device()
        if device is not None:
            self.var_token.set(device.token_hex)
        self._refresh_buttons()

    def _refresh_buttons(self) -> None:
        connected = self.agent is not None
        has_selection = self.selected_device() is not None
        self.btn_connect.config(state="normal" if (has_selection and not connected) else "disabled")
        self.btn_disconnect.config(state="normal" if connected else "disabled")
        self.btn_scan.config(state="disabled" if (connected or self.scanning) else "normal")
        self.btn_relay.config(state="disabled" if connected else "normal")


    def _apply_config_file(self) -> None:
        path = gui_config_path()
        if not path.exists():
            return
        try:
            cfg = json.loads(path.read_text(encoding="utf-8-sig"))
        except Exception:  # noqa: BLE001
            return
        self.var_relay_host.set(str(cfg.get("relay_host", "")))
        self.var_relay_port.set(str(cfg.get("relay_port", 8790)))
        self.var_relay_key.set(str(cfg.get("relay_key", "")))
        self.var_relay_device.set(str(cfg.get("device_id", "")))
        self.var_e2e.set(str(cfg.get("pair_code", "")))
        ca = cfg.get("ca") or default_ca_path()
        self.var_relay_ca.set(str(ca))
        self.hint.set("已读取 kvm-wire.json，可点“连接中继”")
        if cfg.get("auto_connect"):
            self.root.after(1500, lambda: self.connect_relay(False))

    def browse_ca(self) -> None:
        path = filedialog.askopenfilename(title="选择中继 CA 证书", filetypes=[("证书", "*.crt *.pem"), ("全部", "*.*")])
        if path:
            self.var_relay_ca.set(path)

    # ------------------------------------------------------------------
    # 连接 / 断开
    # ------------------------------------------------------------------
    def connect_selected(self) -> None:
        device = self.selected_device()
        if device is None:
            messagebox.showinfo("提示", "请先在列表里选择一个设备")
            return
        if self.agent is not None:
            return
        ok = messagebox.askyesno(
            "确认连接",
            f"连接到：\n\n{device.name or 'ESP32-S3 KVM'}\n{device.addr}:{device.port}\nMAC {device.mac}\n\n"
            "连接后，蓝牙键鼠的操作会注入到本机（当前前台窗口）。\n确定开始吗？",
        )
        if not ok:
            return

        try:
            token = kvmproto.parse_token(self.var_token.get().strip())
        except ValueError as exc:
            messagebox.showerror("Token 错误", str(exc))
            return
        try:
            local_port = int(self.var_local_port.get())
            scale = float(self.var_scale.get())
            wheel = int(self.var_wheel.get())
        except ValueError:
            messagebox.showerror("参数错误", "端口/灵敏度/滚轮倍率必须是数字")
            return

        cfg = AgentConfig(
            port=local_port,
            token=token,
            peer_ip=device.addr,
            peer_port=device.port,
            mouse_mode=self.var_mode.get(),
            mouse_scale=scale,
            wheel_scale=wheel,
            repeat=self.var_repeat.get(),
        )
        self._start_agent(cfg)
        self.hint.set(f"已连接 {device.addr}，键鼠操作已注入本机。")

    def connect_relay(self, confirm: bool = True) -> None:
        if self.agent is not None:
            return
        host = self.var_relay_host.get().strip()
        if not host:
            messagebox.showinfo("提示", "请填写中继地址")
            return
        try:
            port = int(self.var_relay_port.get())
            scale = float(self.var_scale.get())
            wheel = int(self.var_wheel.get())
        except ValueError:
            messagebox.showerror("参数错误", "端口/灵敏度/滚轮倍率必须是数字")
            return
        key = self.var_relay_key.get().strip()
        if not key:
            messagebox.showinfo("提示", "请填写中继客户端密钥")
            return
        ca = self.var_relay_ca.get().strip()
        if not ca or not Path(ca).exists():
            messagebox.showerror("CA 证书错误", "找不到 CA 证书，请选择中继服务器生成的 ca.crt")
            return
        device_id = self.var_relay_device.get().strip() or None
        if confirm and not messagebox.askyesno(
            "确认连接中继",
            f"中继：{host}:{port}\n设备：{device_id or '自动选择第一台'}\n\n"
            "连接后，蓝牙键鼠的操作会注入到本机（当前前台窗口）。\n确定开始吗？",
        ):
            return

        cfg = AgentConfig(
            relay_host=host,
            relay_port=port,
            relay_ca=ca,
            relay_key=key,
            relay_device=device_id,
            e2e_key=self.var_e2e.get().strip() or None,
            mouse_mode=self.var_mode.get(),
            mouse_scale=scale,
            wheel_scale=wheel,
            repeat=self.var_repeat.get(),
        )
        self._start_agent(cfg)
        self.hint.set(f"正在通过中继 {host}:{port} 连接设备…")

    def _start_agent(self, cfg: AgentConfig) -> None:
        self.agent = KvmAgent(cfg)
        self.agent_thread = threading.Thread(target=self._run_agent, name="kvm-agent", daemon=True)
        self.agent_thread.start()
        self.status_vars["state"].set("已连接（注入中）")
        self._refresh_buttons()

    def _run_agent(self) -> None:
        try:
            self.agent.run()
        except SystemExit as exc:
            msg = str(exc)
            self.root.after(0, lambda: self._agent_failed(msg))
        except Exception as exc:  # noqa: BLE001
            msg = f"{type(exc).__name__}: {exc}"
            self.root.after(0, lambda: self._agent_failed(msg))

    def _agent_failed(self, msg: str) -> None:
        if self.agent is not None:
            try:
                self.agent.stop()
            except Exception:  # noqa: BLE001
                pass
        self.agent = None
        self.agent_thread = None
        self.status_vars["state"].set("连接失败")
        self.hint.set(msg)
        self._refresh_buttons()
        messagebox.showerror("连接失败", msg)

    def disconnect(self) -> None:
        if self.agent is None:
            return
        self.agent.stop()
        self.agent = None
        self.agent_thread = None
        self.status_vars["state"].set("已断开")
        self.status_vars["frames"].set("-")
        self.status_vars["lost"].set("-")
        self.status_vars["rtt"].set("-")
        self.hint.set("已断开。可以重新扫描或关闭窗口。")
        self._refresh_buttons()
        self.root.after(600, self.scan_devices)

    def release_all(self) -> None:
        if self.agent is not None:
            self.agent.request_release()
            self.hint.set("已释放全部按键。")
        else:
            self.hint.set("尚未连接设备。")

    # ------------------------------------------------------------------
    # 状态刷新
    # ------------------------------------------------------------------
    def update_status(self) -> None:
        if self.agent is not None:
            st = self.agent.engine.stats
            online = st.last_rx and (time.monotonic() - st.last_rx) < 3.0
            self.status_vars["state"].set("在线（注入中）" if online else "已连接，等待数据…")
            self.status_vars["frames"].set(str(st.frames))
            self.status_vars["lost"].set(str(st.lost))
            rtt = self.agent.rtt_ms
            self.status_vars["rtt"].set(f"{rtt:.0f} ms" if rtt is not None else "-")
        else:
            self.status_vars["state"].set("未连接")
        self.root.after(400, self.update_status)

    def on_close(self) -> None:
        if getattr(self, "_quitting", False):
            return
        self.root.withdraw()
        if self.tray is not None:
            try:
                self.tray.notify("KVM-Wire 仍在后台运行，右键托盘图标可退出", "KVM-Wire")
            except Exception:  # noqa: BLE001
                pass

    # ------------------------------------------------------------------
    # 托盘
    # ------------------------------------------------------------------
    def _setup_tray(self) -> None:
        image = None
        try:
            image = Image.open(resource_path("assets/icon.png")).resize((64, 64))
        except Exception:  # noqa: BLE001
            image = Image.new("RGB", (64, 64), (30, 90, 200))
        menu = pystray.Menu(
            pystray.MenuItem("显示主窗口", self._tray_show, default=True),
            pystray.MenuItem("退出", self._tray_quit),
        )
        self.tray = pystray.Icon("KVM-Wire", image, "KVM-Wire", menu)
        try:
            self.tray.run_detached()
        except Exception:  # noqa: BLE001
            self.tray = None

    def _tray_show(self, _icon=None, _item=None) -> None:
        self.root.after(0, self._show_window)

    def _show_window(self) -> None:
        self.root.deiconify()
        self.root.lift()
        self.root.focus_force()

    def _tray_quit(self, _icon=None, _item=None) -> None:
        self.root.after(0, self._quit_app)

    def _quit_app(self) -> None:
        self._quitting = True
        try:
            if self.agent is not None:
                self.agent.stop()
        finally:
            try:
                self.discovery.close()
            except Exception:  # noqa: BLE001
                pass
            if self.tray is not None:
                try:
                    self.tray.stop()
                except Exception:  # noqa: BLE001
                    pass
            self.root.destroy()


def main() -> int:
    root = tk.Tk()
    sv_ttk.set_theme("light")
    KvmApp(root)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())


