# -*- coding: utf-8 -*-
"""
机房维护系统 MRMS —— 服务端
================================
功能：
  1) 汇总显示所有客户端信息（IP 地址 / 操作系统 / 电脑用户名 / 权限 / 分辨率）
  2) 选中客户端后可远程查看并控制屏幕（鼠标点击 / 键盘输入）
  3) 远程执行 cmd 命令；常用命令保存于同目录 sql.txt（启动时读取，每行一条，
     页面上可快捷选中执行）
配置：
  config.json（同目录），每次启动时读取；服务端默认地址 10.100.4.111:3309
运行：
  python server.py
  若安装了 waitress（pip install waitress）将自动使用，更稳定。

协议（客户端 <-> 服务端，纯文本，字段以 0x1f 分隔，行以 \n 分隔）：
  POST /poll    (form: cid,host,user,os,arch,priv,ver,sw,sh,caps,token)
                注册 + 心跳 + 取命令。返回：
      INTERVAL\x1f<秒>
      EXEC\x1f<id>\x1f<超时秒>\x1f<base64(UTF-8命令)>
      SCREEN\x1f<质量>
      MOUSE\x1f<nx>\x1f<ny>\x1f<move|left|right|dbl>   (nx,ny: 0..10000 归一化)
      KEY\x1f<base64(UTF-8文本)>
      SKEY\x1f<组合键名，如 enter / ctrl+c / alt+f4>
      REBOOT / SHUTDOWN
  POST /result  (form: cid,id,code,output,token)         命令执行结果
  POST /screen  (头: X-CID, X-Token; 体: 原始 JPEG)       屏幕帧
"""
import base64
import json
import os
import re
import secrets
import threading
import time
from datetime import timedelta

from flask import (Flask, Response, jsonify, redirect, render_template,
                   request, session)

BASE = os.path.dirname(os.path.abspath(__file__))
CFG_FILE = os.path.join(BASE, "config.json")
SQL_FILE = os.path.join(BASE, "sql.txt")

DEFAULTS = {
    "server_ip": "10.100.4.111",     # 服务端对外地址（客户端连它）
    "port": 3309,                    # 监听端口
    "listen_host": "0.0.0.0",        # 实际绑定地址
    "admin_password": "mrcs123456",  # 网页登录密码（部署后务必修改）
    "client_token": "MRCS-2026-DEFAULT-TOKEN",  # 客户端接入令牌（部署后务必修改）
    "poll_interval": 5.0,            # 空闲时客户端轮询间隔（秒）
    "view_interval": 0.4,            # 屏幕观看时轮询/推帧间隔（秒）
    "cmd_timeout": 120,              # 远程命令默认超时（秒）
}


# ---------------------------------------------------------------- 配置 / sql.txt
def load_config():
    cfg = dict(DEFAULTS)
    if os.path.exists(CFG_FILE):
        try:
            with open(CFG_FILE, "r", encoding="utf-8-sig") as f:
                user = json.load(f)
            if isinstance(user, dict):
                for k, v in user.items():
                    cfg[k] = v
        except Exception as e:
            print("[配置] config.json 读取失败：%r，使用默认配置" % e)
    if not cfg.get("_secret"):
        cfg["_secret"] = secrets.token_hex(24)
    # 回写，保证文件中始终包含全部字段（首次运行自动生成）
    try:
        with open(CFG_FILE, "w", encoding="utf-8") as f:
            json.dump(dict(cfg), f, ensure_ascii=False, indent=2)
    except Exception:
        pass
    return cfg


def load_sql():
    """读取常用 cmd 命令，每行一条；文件不存在则创建默认内容。"""
    if not os.path.exists(SQL_FILE):
        with open(SQL_FILE, "w", encoding="utf-8") as f:
            f.write("net user Administrator Ab123456\n")
    out = []
    try:
        with open(SQL_FILE, "r", encoding="utf-8-sig") as f:
            for ln in f.read().splitlines():
                ln = ln.strip()
                if ln and not ln.startswith("#"):
                    out.append(ln)
    except Exception as e:
        print("[命令] sql.txt 读取失败：%r" % e)
    return out


CFG = load_config()
SQL_CMDS = load_sql()          # 服务端启动时读取（需求）
POLL_INTERVAL = float(CFG["poll_interval"])
VIEW_INTERVAL = float(CFG["view_interval"])
CMD_TIMEOUT = int(CFG["cmd_timeout"])

# ---------------------------------------------------------------- 全局状态
LOCK = threading.RLock()
CLIENTS = {}    # cid -> 信息字典
RESULTS = {}    # cmd_id -> {"done":bool,"code":int,"output":str,"ts":float,"cmd":str}
HISTORY = {}    # cid -> [{"ts","cmd","code","output"}] 最近若干条
CID_RE = re.compile(r"^[A-Za-z0-9_\-]{1,64}$")
LOGIN_FAILS = {}  # ip -> [失败次数, 封禁截止时间]


def new_client(cid):
    return {
        "cid": cid, "host": cid, "user": "", "os": "", "arch": "",
        "priv": "0", "ver": "", "sw": "0", "sh": "0", "caps": "",
        "ip": "", "first_seen": time.time(), "last_seen": 0.0,
        "viewing": False, "boost_until": 0.0,
        "q": [],                  # 待下发命令行列表
        "frame": None, "frame_ts": 0.0,
    }


def client_online(rec):
    thr = max(10.0, POLL_INTERVAL * 3)
    return (time.time() - rec["last_seen"]) < thr


def enqueue(cid, line):
    """命令入队；有新命令时让客户端加快轮询。失败返回 False。"""
    with LOCK:
        rec = CLIENTS.get(cid)
        if rec is None:
            return False
        rec["q"].append(line)
        rec["boost_until"] = time.time() + 3.0
        return True


def enqueue_mouse(cid, nx, ny, action):
    with LOCK:
        rec = CLIENTS.get(cid)
        if rec is None:
            return False
        q = rec["q"]
        # move 事件只保留最新一条，避免堆积
        if action == "move" and q and q[-1].startswith("MOUSE") and q[-1].endswith("\x1fmove"):
            q.pop()
        q.append("MOUSE\x1f%d\x1f%d\x1f%s" % (nx, ny, action))
        rec["boost_until"] = time.time() + 3.0
        return True


def sweep():
    """在锁内清理过期帧 / 超量结果与历史。"""
    now = time.time()
    if len(RESULTS) > 1000:
        for k in sorted(RESULTS, key=lambda k: RESULTS[k].get("ts", 0))[:len(RESULTS) - 600]:
            RESULTS.pop(k, None)
    for rec in CLIENTS.values():
        if rec["frame"] is not None and now - rec["frame_ts"] > 180:
            rec["frame"] = None
        hist = HISTORY.get(rec["cid"])
        if hist and len(hist) > 30:
            del hist[:len(hist) - 20]


# ---------------------------------------------------------------- Flask 应用
app = Flask(__name__)
app.config["MAX_CONTENT_LENGTH"] = 16 * 1024 * 1024
app.secret_key = CFG.get("_secret")
app.permanent_session_lifetime = timedelta(days=7)


def check_client_token():
    tok = request.form.get("token") or request.headers.get("X-Token") or ""
    want = str(CFG["client_token"])
    return secrets.compare_digest(tok, want)


@app.before_request
def gate():
    p = request.path
    if p in ("/poll", "/result", "/screen", "/login"):
        return None
    if p == "/favicon.ico":
        return ("", 204)
    if not session.get("auth"):
        if p.startswith("/api/"):
            return jsonify({"ok": False, "err": "unauthorized"}), 401
        return redirect("/login")
    return None


# ---------------------------------------------------------------- 客户端接口
@app.post("/poll")
def client_poll():
    if not check_client_token():
        return "ERR token", 403
    f = request.form
    cid = (f.get("cid") or "").strip()
    if not CID_RE.match(cid):
        return "ERR cid", 400
    now = time.time()
    with LOCK:
        rec = CLIENTS.get(cid)
        if rec is None:
            rec = new_client(cid)
            CLIENTS[cid] = rec
        for k in ("host", "user", "os", "arch", "priv", "ver", "sw", "sh", "caps"):
            v = f.get(k)
            if v is not None:
                rec[k] = str(v)[:128]
        rec["ip"] = (request.remote_addr or "")[:64]
        rec["last_seen"] = now
        cmds, rec["q"] = rec["q"], []
        if rec["viewing"]:
            interval = VIEW_INTERVAL
            # 观看状态下：先执行队列中的输入命令，末尾追加一条抓帧指令，
            # 客户端回传一帧后立刻再来轮询 —— 由服务端驱动的连续屏幕流。
            cmds = cmds + ["SCREEN\x1f50"]
        elif rec["boost_until"] > now:
            interval = 0.3
        else:
            interval = POLL_INTERVAL
        sweep()
    body = "INTERVAL\x1f%.2f\n" % interval + "".join(c + "\n" for c in cmds)
    return Response(body, mimetype="text/plain")


@app.post("/result")
def client_result():
    if not check_client_token():
        return "ERR token", 403
    cid = (request.form.get("cid") or "").strip()
    cmd_id = (request.form.get("id") or "").strip()
    if not CID_RE.match(cid) or not cmd_id:
        return "ERR", 400
    try:
        code = int(request.form.get("code", "-1"))
    except ValueError:
        code = -1
    output = (request.form.get("output") or "")[:262144]
    now = time.time()
    with LOCK:
        ent = RESULTS.get(cmd_id)
        if ent is not None:
            ent.update({"done": True, "code": code, "output": output, "ts": now})
            cmd = ent.get("cmd", "")
        else:
            RESULTS[cmd_id] = {"done": True, "code": code, "output": output,
                               "ts": now, "cmd": ""}
            cmd = ""
        rec = CLIENTS.get(cid)
        if rec is not None:
            HISTORY.setdefault(cid, []).append(
                {"ts": now, "cmd": cmd, "code": code, "output": output[:65536]})
            hist = HISTORY[cid]
            if len(hist) > 30:
                del hist[:len(hist) - 20]
    return "OK"


@app.post("/screen")
def client_screen():
    if not check_client_token():
        return "ERR token", 403
    cid = (request.headers.get("X-CID") or "").strip()
    if not CID_RE.match(cid):
        return "ERR cid", 400
    data = request.get_data(cache=False)
    if not data or len(data) > 12 * 1024 * 1024:
        return "ERR data", 400
    with LOCK:
        rec = CLIENTS.get(cid)
        if rec is not None:
            rec["frame"] = data
            rec["frame_ts"] = time.time()
    return "OK"


# ---------------------------------------------------------------- 登录
@app.route("/login", methods=["GET", "POST"])
def login():
    err = ""
    if request.method == "POST":
        pw = request.form.get("password", "")
        ip = (request.remote_addr or "?")
        now = time.time()
        with LOCK:
            cnt, until = LOGIN_FAILS.get(ip, (0, 0))
        if until > now:
            err = "失败次数过多，请稍后再试"
        elif secrets.compare_digest(pw, str(CFG["admin_password"])):
            session["auth"] = True
            session.permanent = True
            with LOCK:
                LOGIN_FAILS.pop(ip, None)
            return redirect("/")
        else:
            with LOCK:
                LOGIN_FAILS[ip] = (cnt + 1, now + 30 if cnt + 1 >= 8 else 0)
            err = "密码错误"
    return render_template("login.html", err=err)


@app.route("/logout")
def logout():
    session.clear()
    return redirect("/login")


@app.get("/")
def index():
    return render_template("index.html",
                           srv="%s:%s" % (CFG["server_ip"], CFG["port"]))


# ---------------------------------------------------------------- 管理端 API
@app.get("/api/clients")
def api_clients():
    with LOCK:
        items = []
        for rec in CLIENTS.values():
            items.append({
                "cid": rec["cid"], "host": rec["host"], "ip": rec["ip"],
                "user": rec["user"], "os": rec["os"], "arch": rec["arch"],
                "priv": rec["priv"], "ver": rec["ver"],
                "sw": rec["sw"], "sh": rec["sh"], "caps": rec["caps"],
                "online": client_online(rec),
                "last_seen": round(rec["last_seen"], 1),
                "viewing": rec["viewing"],
                "pending": len(rec["q"]),
            })
    items.sort(key=lambda x: (not x["online"], x["host"].lower()))
    return jsonify({"ok": True, "clients": items, "now": time.time()})


@app.post("/api/exec")
def api_exec():
    d = request.get_json(silent=True) or {}
    cid = (d.get("cid") or "").strip()
    cmd = (d.get("cmd") or "").strip()
    if not CID_RE.match(cid) or not cmd or len(cmd) > 4000:
        return jsonify({"ok": False, "err": "参数无效"}), 400
    cmd_id = secrets.token_hex(6)
    line = "EXEC\x1f%s\x1f%d\x1f%s" % (cmd_id, CMD_TIMEOUT,
                                       base64.b64encode(cmd.encode("utf-8")).decode())
    if not enqueue(cid, line):
        return jsonify({"ok": False, "err": "客户端不存在或从未上线"}), 404
    with LOCK:
        RESULTS[cmd_id] = {"done": False, "code": 0, "output": "",
                           "ts": time.time(), "cmd": cmd}
    return jsonify({"ok": True, "id": cmd_id})


@app.get("/api/result")
def api_result():
    cmd_id = (request.args.get("id") or "").strip()
    with LOCK:
        ent = RESULTS.get(cmd_id)
        if ent is None:
            return jsonify({"ok": False, "err": "not found"}), 404
        return jsonify({"ok": True, "done": ent["done"], "code": ent["code"],
                        "output": ent["output"], "cmd": ent.get("cmd", ""),
                        "ts": ent["ts"]})


@app.get("/api/history")
def api_history():
    cid = (request.args.get("cid") or "").strip()
    if not CID_RE.match(cid):
        return jsonify({"ok": False, "err": "参数无效"}), 400
    with LOCK:
        items = list(reversed(HISTORY.get(cid, [])))
    return jsonify({"ok": True, "items": items})


@app.post("/api/view")
def api_view():
    d = request.get_json(silent=True) or {}
    cid = (d.get("cid") or "").strip()
    on = bool(d.get("on"))
    with LOCK:
        rec = CLIENTS.get(cid)
        if rec is None:
            return jsonify({"ok": False, "err": "客户端不存在"}), 404
        rec["viewing"] = on
        if on:
            rec["boost_until"] = time.time() + 3.0
    return jsonify({"ok": True})


@app.get("/api/frame")
def api_frame():
    cid = (request.args.get("cid") or "").strip()
    with LOCK:
        rec = CLIENTS.get(cid)
        data = rec["frame"] if rec else None
    if not data:
        return Response("no frame", mimetype="text/plain", status=404)
    return Response(data, mimetype="image/jpeg",
                    headers={"Cache-Control": "no-store"})


@app.post("/api/input")
def api_input():
    d = request.get_json(silent=True) or {}
    cid = (d.get("cid") or "").strip()
    kind = d.get("kind")
    ok = False
    if kind == "mouse":
        try:
            nx = min(10000, max(0, int(d.get("nx", 0))))
            ny = min(10000, max(0, int(d.get("ny", 0))))
        except (TypeError, ValueError):
            return jsonify({"ok": False, "err": "参数无效"}), 400
        action = d.get("action", "move")
        if action not in ("move", "left", "right", "dbl", "ldown", "lup", "rdown", "rup"):
            action = "move"
        ok = enqueue_mouse(cid, nx, ny, action)
    elif kind == "key":
        text = str(d.get("text", ""))[:512]
        if text:
            ok = enqueue(cid, "KEY\x1f" + base64.b64encode(
                text.encode("utf-8")).decode())
    elif kind == "skey":
        name = str(d.get("name", ""))[:64]
        if name:
            ok = enqueue(cid, "SKEY\x1f" + name)
    if not ok:
        return jsonify({"ok": False, "err": "客户端不存在"}), 404
    return jsonify({"ok": True})


@app.post("/api/power")
def api_power():
    d = request.get_json(silent=True) or {}
    cid = (d.get("cid") or "").strip()
    action = d.get("action")
    if action not in ("reboot", "shutdown"):
        return jsonify({"ok": False, "err": "参数无效"}), 400
    if not enqueue(cid, "REBOOT" if action == "reboot" else "SHUTDOWN"):
        return jsonify({"ok": False, "err": "客户端不存在"}), 404
    with LOCK:
        HISTORY.setdefault(cid, []).append(
            {"ts": time.time(), "cmd": "【电源】" + ("重启" if action == "reboot" else "关机"),
             "code": 0, "output": ""})
    return jsonify({"ok": True})


@app.get("/api/commands")
def api_commands():
    # 启动时已读取过；这里每次刷新，编辑 sql.txt 后无需重启
    try:
        cmds = load_sql()
    except Exception:
        cmds = SQL_CMDS
    return jsonify({"ok": True, "cmds": cmds})


# ---------------------------------------------------------------- 启动
def main():
    print("=" * 60)
    print(" 机房维护系统 MRMS 服务端")
    print(" 访问地址   : http://%s:%s" % (CFG["server_ip"], CFG["port"]))
    print(" 实际监听   : %s:%s" % (CFG["listen_host"], CFG["port"]))
    print(" 管理密码   : 见 config.json 中 admin_password（默认 mrcs123456）")
    print(" 常用命令   : sql.txt 已加载 %d 条" % len(SQL_CMDS))
    for c in SQL_CMDS:
        print("    - %s" % c)
    print("=" * 60)
    try:
        import waitress
        waitress.serve(app, host=str(CFG["listen_host"]),
                       port=int(CFG["port"]), threads=32)
    except ImportError:
        app.run(host=str(CFG["listen_host"]), port=int(CFG["port"]),
                threaded=True, debug=False, use_reloader=False)


if __name__ == "__main__":
    main()
