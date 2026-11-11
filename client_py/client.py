#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
机房维护系统 MRMS —— Python 备用客户端
=====================================
与 C++ 客户端（MRMSClient.cpp）协议完全一致，作为备用/参考实现。

系统要求：
  Windows 10       : Python 3.8 ~ 3.12，可选 pillow（无 pillow 时屏幕功能禁用）
  Windows 7 无SP   : Python 3.8.x + pillow<=9.5（建议这类机器优先使用 C++ 客户端）
  其他系统（测试） : 仅命令执行通道可用

运行方式：
  pythonw client.py            后台常驻（无黑窗）
  python  client.py            前台运行（调试）
  python  client.py --install  仅写入自启动后退出
命令行参数可覆盖 client_config.json：
  --server http://10.100.4.111:5309
  --token MRCS-2026-DEFAULT-TOKEN
  --once   只执行一轮轮询（测试用）
"""
import argparse
import base64
import json
import os
import platform
import secrets
import socket
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

IS_WIN = sys.platform == "win32"
if IS_WIN:
    import ctypes
    import winreg
    import getpass

VERSION = "1.0"
THIS_DIR = os.path.dirname(os.path.abspath(__file__))
CFG_FILE = os.path.join(THIS_DIR, "client_config.json")
ID_FILE = os.path.join(THIS_DIR, "client.id")
LOG_FILE = os.path.join(THIS_DIR, "mrms_client.log")

SERVER = "http://10.100.4.111:3309"
TOKEN = "MRCS-2026-DEFAULT-TOKEN"
LOG_ON = True
INTERVAL = 5.0

try:
    from PIL import ImageGrab, Image  # noqa
    HAVE_PIL = True
except Exception:
    HAVE_PIL = False

if IS_WIN:
    ULONG_PTR = ctypes.POINTER(ctypes.c_ulong)
    MOUSEEVENTF_MOVE = 0x0001
    MOUSEEVENTF_LEFTDOWN = 0x0002
    MOUSEEVENTF_LEFTUP = 0x0004
    MOUSEEVENTF_RIGHTDOWN = 0x0008
    MOUSEEVENTF_RIGHTUP = 0x0010
    MOUSEEVENTF_ABSOLUTE = 0x8000
    MOUSEEVENTF_VIRTUALDESK = 0x4000
    KEYEVENTF_KEYUP = 0x0002
    KEYEVENTF_UNICODE = 0x0004

    class _MOUSEINPUT(ctypes.Structure):
        _fields_ = [("dx", ctypes.c_long), ("dy", ctypes.c_long),
                    ("mouseData", ctypes.c_ulong),
                    ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                    ("dwExtraInfo", ULONG_PTR)]

    class _KEYBDINPUT(ctypes.Structure):
        _fields_ = [("wVk", ctypes.c_ushort), ("wScan", ctypes.c_ushort),
                    ("dwFlags", ctypes.c_ulong), ("time", ctypes.c_ulong),
                    ("dwExtraInfo", ULONG_PTR)]

    class _HARDWAREINPUT(ctypes.Structure):
        _fields_ = [("uMsg", ctypes.c_ulong), ("wParamL", ctypes.c_ushort),
                    ("wParamH", ctypes.c_ushort)]

    class _INPUTunion(ctypes.Union):
        _fields_ = [("mi", _MOUSEINPUT), ("ki", _KEYBDINPUT),
                    ("hi", _HARDWAREINPUT)]

    class _INPUT(ctypes.Structure):
        _fields_ = [("type", ctypes.c_ulong), ("union", _INPUTunion)]

    def _send_input(inp):
        ctypes.windll.user32.SendInput(1, ctypes.byref(inp), ctypes.sizeof(inp))

    VK_MAP = {
        "ctrl": 0x11, "alt": 0x12, "shift": 0x10, "win": 0x5B,
        "enter": 0x0D, "esc": 0x1B, "tab": 0x09, "bksp": 0x08,
        "space": 0x20, "del": 0x2E, "ins": 0x2D, "home": 0x24, "end": 0x23,
        "pgup": 0x21, "pgdn": 0x22, "up": 0x26, "down": 0x28,
        "left": 0x25, "right": 0x27,
    }

    def vk_of(name):
        n = name.lower().strip()
        if n in VK_MAP:
            return VK_MAP[n]
        if n.startswith("f") and n[1:].isdigit() and 1 <= int(n[1:]) <= 12:
            return 0x70 + int(n[1:]) - 1
        if len(n) == 1:
            return ord(n.upper())
        return None

CID = ""
HOST = ""
USER = ""
OS_STR = ""
ARCH = ""
PRIV = "0"
SW = SH = "0"
CAPS = "noscreen" if not HAVE_PIL else "screen"


def log(msg):
    line = "[%s] %s" % (time.strftime("%Y-%m-%d %H:%M:%S"), msg)
    print(line)
    if not LOG_ON:
        return
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(line + "\n")
        if os.path.getsize(LOG_FILE) > 512 * 1024:
            open(LOG_FILE, "w").close()
    except OSError:
        pass


def load_config():
    global SERVER, TOKEN, LOG_ON
    try:
        with open(CFG_FILE, "r", encoding="utf-8-sig") as f:
            cfg = json.load(f)
        if cfg.get("server"):
            SERVER = cfg["server"]
        if cfg.get("token"):
            TOKEN = cfg["token"]
        if "log" in cfg:
            LOG_ON = str(cfg["log"]).lower() not in ("false", "0")
    except Exception:
        pass


def http_post(path, form=None, raw=None, headers=None):
    url = SERVER.rstrip("/") + path
    h = dict(headers or {})
    if form is not None:
        data = urllib.parse.urlencode(form).encode()
        h.setdefault("Content-Type", "application/x-www-form-urlencoded")
    else:
        data = raw or b""
        h.setdefault("Content-Type", "application/octet-stream")
    req = urllib.request.Request(url, data=data, headers=h)
    with urllib.request.urlopen(req, timeout=20) as resp:
        return resp.status, resp.read().decode("utf-8", "replace")


# ---------------------------------------------------------------- 信息采集
def is_admin():
    try:
        if IS_WIN:
            return bool(ctypes.windll.shell32.IsUserAnAdmin())
        return os.geteuid() == 0
    except Exception:
        return False


def load_or_create_id():
    try:
        with open(ID_FILE, "r") as f:
            s = f.read().strip()
            if s:
                return s
    except OSError:
        pass
    s = secrets.token_hex(4)
    try:
        with open(ID_FILE, "w") as f:
            f.write(s)
    except OSError:
        pass
    return s


def setup():
    global CID, HOST, USER, OS_STR, ARCH, PRIV, SW, SH, CAPS
    # DPI 感知：否则高分屏缩放(125%/150%)下 GDI 虚拟化会导致
    # 分辨率上报/截图/鼠标坐标三者不一致（画面缺边、点击偏移）
    if IS_WIN:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass
    HOST = socket.gethostname()
    try:
        USER = getpass.getuser() if IS_WIN else os.environ.get("USER") or "user"
    except Exception:
        USER = "user"
    raw = "".join(c for c in HOST if c.isalnum() or c in "-_")[:24] or "PC"
    CID = "%s-%s" % (raw, load_or_create_id())
    if IS_WIN:
        OS_STR = platform.system() + " " + " ".join(
            platform.win32_ver()[:2] if hasattr(platform, "win32_ver") else [])
        OS_STR = (platform.platform(terse=True) or "Windows")
        ARCH = "x64" if platform.machine().endswith("64") else "x86"
        try:
            import ctypes.wintypes  # noqa
            user32 = ctypes.windll.user32
            SW = str(user32.GetSystemMetrics(78) or user32.GetSystemMetrics(0))
            SH = str(user32.GetSystemMetrics(79) or user32.GetSystemMetrics(1))
        except Exception:
            SW, SH = "0", "0"
    else:
        OS_STR = platform.platform(terse=True) or "unknown"
        ARCH = platform.machine() or "unknown"
        SW, SH = "0", "0"
    PRIV = "1" if is_admin() else "0"
    CAPS = "screen" if (HAVE_PIL and IS_WIN) else "noscreen"
    log("info: cid=%s host=%s user=%s os=%s priv=%s caps=%s" %
        (CID, HOST, USER, OS_STR, PRIV, CAPS))


# ---------------------------------------------------------------- 自启动
def reg_set_run():
    """把当前脚本注册进开机启动项（每次启动都会执行）。"""
    if not IS_WIN:
        return
    name = "MRMS_ClientPy"
    exe = sys.executable
    pyw = exe.replace("python.exe", "pythonw.exe")
    if os.path.exists(pyw):
        exe = pyw
    val = '"%s" "%s"' % (exe, os.path.abspath(__file__))
    root, key = (winreg.HKEY_LOCAL_MACHINE, None)
    try:
        if is_admin():
            k = winreg.CreateKeyEx(winreg.HKEY_LOCAL_MACHINE,
                                   r"Software\Microsoft\Windows\CurrentVersion\Run",
                                   0, winreg.KEY_SET_VALUE)
        else:
            k = winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER,
                                   r"Software\Microsoft\Windows\CurrentVersion\Run",
                                   0, winreg.KEY_SET_VALUE)
        winreg.SetValueEx(k, name, 0, winreg.REG_SZ, val)
        k.Close()
        log("startup entry written (%s)" %
            ("HKLM" if is_admin() else "HKCU"))
    except OSError as e:
        log("write startup failed: %r" % e)


def reg_del_run():
    for root in (winreg.HKEY_LOCAL_MACHINE, winreg.HKEY_CURRENT_USER):
        try:
            with winreg.OpenKey(root, r"Software\Microsoft\Windows\CurrentVersion\Run",
                                0, winreg.KEY_SET_VALUE) as k:
                winreg.DeleteValue(k, "MRMS_ClientPy")
        except OSError:
            pass


# ---------------------------------------------------------------- 远程命令
def run_cmd(cmd, timeout):
    """执行命令并返回 (输出UTF-8, 退出码)。超时返回码 -1。"""
    try:
        if IS_WIN:
            p = subprocess.run(["cmd.exe", "/c", cmd], capture_output=True,
                               timeout=timeout, creationflags=0x08000000)
        else:
            p = subprocess.run(["/bin/sh", "-c", cmd], capture_output=True,
                              timeout=timeout)
        out = (p.stdout or b"") + (p.stderr or b"")
        try:
            if IS_WIN:
                oem = ctypes.windll.kernel32.GetOEMCP()
                enc = "cp%d" % oem
            else:
                enc = "utf-8"
            text = out.decode(enc, "replace")
        except Exception:
            text = out.decode("utf-8", "replace")
        return text, p.returncode
    except subprocess.TimeoutExpired:
        return "(MRMS) 命令超时被终止", -1
    except Exception as e:
        return "(MRMS) 执行失败: %r" % e, -1


# ---------------------------------------------------------------- 屏幕与输入
def capture_jpeg(quality=50):
    if not HAVE_PIL or not IS_WIN:
        return None
    try:
        # all_screens=True：抓整个虚拟桌面（多显示器时不缺副屏）
        try:
            img = ImageGrab.grab(all_screens=True)
        except TypeError:              # pillow < 6.2 无此参数
            img = ImageGrab.grab()
        import io
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=quality)
        return buf.getvalue()
    except Exception as e:
        log("capture failed: %r" % e)
        return None


def mouse_action(nx, ny, action):
    if not IS_WIN:
        return
    dx = int(nx * 65535 / 10000)
    dy = int(ny * 65535 / 10000)

    def mv():
        inp = _INPUT()
        inp.type = 0
        inp.union.mi.dx = dx
        inp.union.mi.dy = dy
        inp.union.mi.dwFlags = (MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                                MOUSEEVENTF_VIRTUALDESK)
        _send_input(inp)

    def click(down, up):
        for flag in (down, up):
            inp = _INPUT()
            inp.type = 0
            inp.union.mi.dwFlags = flag
            _send_input(inp)

    mv()
    if action == "left":
        click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP)
    elif action == "right":
        click(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP)
    elif action == "dbl":
        click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP)
        time.sleep(0.05)
        click(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP)
    # 按住拖动：按下与抬高分开发送，中间穿插 move 即形成拖拽
    elif action in ("ldown", "lup", "rdown", "rup"):
        flag = {"ldown": MOUSEEVENTF_LEFTDOWN, "lup": MOUSEEVENTF_LEFTUP,
                "rdown": MOUSEEVENTF_RIGHTDOWN, "rup": MOUSEEVENTF_RIGHTUP}[action]
        inp = _INPUT()
        inp.type = 0
        inp.union.mi.dwFlags = flag
        _send_input(inp)


def key_text(text):
    if not IS_WIN:
        return
    for ch in text:
        inp = _INPUT()
        inp.type = 1
        inp.union.ki.wScan = ord(ch)
        inp.union.ki.dwFlags = KEYEVENTF_UNICODE
        _send_input(inp)
        inp2 = _INPUT()
        inp2.type = 1
        inp2.union.ki.wScan = ord(ch)
        inp2.union.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP
        _send_input(inp2)


def key_special(name):
    if not IS_WIN:
        return
    parts = [p for p in name.lower().split("+") if p]
    if not parts:
        return
    mods = [vk_of(p) for p in parts[:-1]]
    main = vk_of(parts[-1])
    if main is None or any(m is None for m in mods):
        log("unknown skey: %s" % name)
        return
    seq = [(m, False) for m in mods] + [(main, True)] + [(m, True) for m in mods]
    for vk, up in seq:
        inp = _INPUT()
        inp.type = 1
        inp.union.ki.wVk = vk
        inp.union.ki.dwFlags = KEYEVENTF_KEYUP if up else 0
        _send_input(inp)
        time.sleep(0.01)


def power(shutdown):
    if IS_WIN:
        flag = "/s" if shutdown else "/r"
        subprocess.Popen("shutdown %s /t 3 /f" % flag, shell=True,
                         creationflags=0x08000000)
    else:
        subprocess.Popen("reboot" if not shutdown else "poweroff", shell=True)


# ---------------------------------------------------------------- 轮询主循环
def poll_once():
    global INTERVAL
    form = {"cid": CID, "host": HOST, "user": USER, "os": OS_STR,
            "arch": ARCH, "priv": PRIV, "ver": VERSION, "sw": SW, "sh": SH,
            "caps": CAPS, "token": TOKEN}
    st, body = http_post("/poll", form=form)
    for line in body.split("\n"):
        line = line.strip()
        if not line:
            continue
        f = line.split("\x1f")
        tag = f[0]
        if tag == "INTERVAL" and len(f) > 1:
            try:
                v = float(f[1])
                if 0.15 <= v <= 300:
                    INTERVAL = v
            except ValueError:
                pass
        elif tag == "EXEC" and len(f) >= 4:
            timeout = int(f[2])
            cmd = base64.b64decode(f[3]).decode("utf-8")
            out, code = run_cmd(cmd, timeout)
            try:
                http_post("/result", form={"cid": CID, "id": f[1],
                                          "code": str(code),
                                          "output": out[:262144],
                                          "token": TOKEN})
            except Exception as e:
                log("post result failed: %r" % e)
        elif tag == "SCREEN":
            jpeg = capture_jpeg()
            if jpeg:
                try:
                    http_post("/screen", raw=jpeg,
                             headers={"X-CID": CID, "X-Token": TOKEN})
                except Exception as e:
                    log("post screen failed: %r" % e)
        elif tag == "MOUSE" and len(f) >= 4:
            mouse_action(int(f[1]), int(f[2]), f[3])
            # 按键类事件间留间隔，确保程序能识别到“按住”
            if f[3] in ("ldown", "lup", "left", "right", "dbl", "rdown", "rup"):
                time.sleep(0.015)
        elif tag == "KEY" and len(f) >= 2:
            key_text(base64.b64decode(f[1]).decode("utf-8"))
        elif tag == "SKEY" and len(f) >= 2:
            key_special(f[1])
        elif tag == "REBOOT":
            power(False)
        elif tag == "SHUTDOWN":
            power(True)
    return True


def main():
    global SERVER, TOKEN
    ap = argparse.ArgumentParser()
    ap.add_argument("--server", help="服务端地址，如 http://10.100.4.111:3309")
    ap.add_argument("--token", help="接入令牌")
    ap.add_argument("--install", action="store_true", help="仅安装自启动后退出")
    ap.add_argument("--uninstall", action="store_true", help="移除自启动")
    ap.add_argument("--once", action="store_true", help="只轮询一轮（测试）")
    args = ap.parse_args()
    load_config()
    if args.server:
        SERVER = args.server
    if args.token:
        TOKEN = args.token
    setup()
    if args.uninstall:
        reg_del_run()
        log("uninstalled startup entry")
        return
    reg_set_run()  # 每次启动都重新写入开机启动项
    if args.install:
        log("installed, exit")
        return
    log("MRMS python client %s started cid=%s server=%s" %
        (VERSION, CID, SERVER))
    fails = 0
    while True:
        try:
            poll_once()
            fails = 0
        except Exception as e:
            fails += 1
            log("poll failed #%d: %r" % (fails, e))
        if args.once:
            break
        time.sleep(INTERVAL if not fails else min(30, 3 + fails))


if __name__ == "__main__":
    main()
