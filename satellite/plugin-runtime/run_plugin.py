"""Runs one JavaScript plugin on this computer against a stand-in for the firmware (no sandbox), for its author:
it answers the plugin's requests (storage in memory, real HTTP(S), play/stop only printed) and calls one of its commands.
Usage: python3 run_plugin.py <lostos-js> <main.js> [<command> '<json args>']   (prefix the binary with qemu-arm for the ARM build)"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

args = sys.argv[1:]
prefix = []
if args and args[0].startswith("qemu"):
    prefix, args = [args[0]], args[1:]
runner, script = args[0], args[1]
tool, tool_args = (args[2], json.loads(args[3]) if len(args) > 3 else {}) if len(args) > 2 else (None, None)

fw, pl = socket.socketpair()
os.set_inheritable(pl.fileno(), True)
proc = subprocess.Popen(prefix + [runner], preexec_fn=lambda: os.dup2(pl.fileno(), 3), close_fds=False)
pl.close()
src = open(script, "rb").read()
fw.sendall(b"! script %d\n" % len(src) + src)
kv, buf, called, answer = {}, b"", False, None
fw.settimeout(0.3)
opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
end = time.time() + 40
while time.time() < end and answer is None:
    try:
        chunk = fw.recv(1 << 20)
        if not chunk:
            break
        buf += chunk
    except socket.timeout:
        pass
    while b"\n" in buf:
        line, buf = buf.split(b"\n", 1)
        n, _, rest = line.decode().partition(" ")
        method, _, a = rest.partition(" ")
        out = "ok"
        if method == "log":
            print("log:", a)
            if a == "started" and tool and not called:
                called = True
                fw.sendall(("! tool " + json.dumps({"id": "t1", "name": tool, "args": tool_args}, ensure_ascii=False) + "\n").encode())
            if n == "0":
                continue
        elif method == "get":
            out = "ok " + kv[a] if a in kv else "ok"
        elif method == "set":
            k, _, v = a.partition(" ")
            kv[k] = v
        elif method == "http":
            d = json.loads(a)
            req = urllib.request.Request(d["url"], data=d["body"].encode() if d["body"] else None, method=d["method"], headers=d.get("headers") or {})
            try:
                with opener.open(req, timeout=10) as r:
                    out = "ok " + json.dumps({"status": r.status, "type": r.headers.get("Content-Type", ""), "body": r.read(256 << 10).decode("utf-8", "replace")}, ensure_ascii=False)
            except urllib.error.HTTPError as e:
                out = "ok " + json.dumps({"status": e.code, "type": "", "body": ""})
            except Exception as e:
                out = "err " + str(e)[:200]
            print(f"http {d['method']} {d['url'][:100]} -> {out[:60]}")
        elif method in ("play", "stop"):
            print(method, a)
        elif method == "tool":
            print("command:", json.loads(a)["name"])
        elif method == "toolresult":
            answer = json.loads(a)["result"]
        elif method == "subscribe":
            pass
        else:
            out = "err unknown method"
        fw.sendall(f"{n} {out}\n".encode())
print("answer:", answer)
proc.kill()
sys.exit(0 if (answer is not None or tool is None) else 1)
