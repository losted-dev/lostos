"""Host test of lostos-js: plays the firmware's side of the plugin protocol (plugins/host.h) over fd 3, without the sandbox.
Runs a test plugin through the whole API: logs, timers, events, storage, a command for the assistant, real HTTP, playing, a runaway
loop that must be interrupted. Usage: python3 test_runtime.py <lostos-js binary> [qemu-arm]   (exit 0 = everything passed)"""
import json
import os
import socket
import subprocess
import sys
import time
import urllib.request

RUNNER = sys.argv[1]
PREFIX = sys.argv[2:]
OK = True

SCRIPT = r"""
console.log("hello from js", 1 + 1);
await lostos.storage.set("visits", "1");
const v = await lostos.storage.get("visits");
lostos.log("storage got", v);
setTimeout(() => lostos.log("timer fired"), 50);
lostos.on("button", (b) => lostos.log("button event", b));
lostos.on("media", (m) => lostos.log("media event", m.source, m.playing, m.title));
await lostos.tool({ name: "add_numbers", description: "Складывает два числа", parameters: { a: { type: "number", description: "первое" }, b: "второе", note: { type: "string", optional: true } } },
  async ({ a, b }) => `сумма ${a + Number(b)}`);
await lostos.tool({ name: "spin", description: "крутится вечно" }, () => { for (;;) {} });
await lostos.tool({ name: "fetch_test", description: "HTTP" }, async () => {
  const r = await lostos.http("http://127.0.0.1:" + globalThis.PORT + "/data.json");
  const j = r.json();
  await lostos.play(j.url, { title: j.title });
  return `got ${r.status} ${j.title}`;
});
globalThis.PORT = await lostos.storage.get("port");
"""


def check(name, cond, extra=""):
    global OK
    print(("PASS " if cond else "FAIL ") + name + ("" if cond else f"  {extra}"))
    OK = OK and bool(cond)


def main():
    # a tiny http server for the http test
    import threading
    from http.server import BaseHTTPRequestHandler, HTTPServer

    class H(BaseHTTPRequestHandler):
        def do_GET(self):
            body = json.dumps({"url": "http://radio.example/stream.mp3", "title": "Тест-радио"}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def log_message(self, *a):
            pass

    srv = HTTPServer(("127.0.0.1", 0), H)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()

    fw, pl = socket.socketpair()
    os.set_inheritable(pl.fileno(), True)

    def child_fd3():
        os.dup2(pl.fileno(), 3)

    proc = subprocess.Popen(PREFIX + [RUNNER], pass_fds=(), preexec_fn=child_fd3, close_fds=False)
    pl.close()
    src = SCRIPT.encode()
    fw.sendall(b"! script %d\n" % len(src) + src)
    kv = {"port": str(port)}
    logs, tools, results, plays, subs = [], {}, {}, [], []
    buf = b""
    sent_events = False
    calls_sent = set()
    deadline = time.time() + 30
    fw.settimeout(0.2)

    def reply(n, res):
        fw.sendall(f"{n} {res}\n".encode())

    while time.time() < deadline:
        try:
            chunk = fw.recv(65536)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            pass
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.decode()
            n, _, rest = s.partition(" ")
            method, _, args = rest.partition(" ")
            if method == "log":
                logs.append(args)
                if n != "0":
                    reply(n, "ok")
            elif method == "set":
                k, _, val = args.partition(" ")
                kv[k] = val
                reply(n, "ok")
            elif method == "get":
                reply(n, "ok " + kv.get(args, "") if kv.get(args) else "ok")
            elif method == "subscribe":
                subs.append(args)
                reply(n, "ok")
            elif method == "tool":
                d = json.loads(args)
                tools[d["name"]] = d
                reply(n, "ok")
            elif method == "toolresult":
                d = json.loads(args)
                results[d["id"]] = d["result"]
                reply(n, "ok")
            elif method == "http":
                d = json.loads(args)
                with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(urllib.request.Request(d["url"], method=d["method"]), timeout=5) as r:
                    body = r.read().decode()
                    reply(n, "ok " + json.dumps({"status": r.status, "type": r.headers.get("Content-Type", ""), "body": body}, ensure_ascii=False))
            elif method == "play":
                plays.append(json.loads(args))
                reply(n, "ok")
            else:
                reply(n, "err unknown method")
        if "started" in logs and not sent_events:
            sent_events = True
            fw.sendall(b"! button action\n! media spotify 1 Song Title\n")
            fw.sendall(("! tool " + json.dumps({"id": "c1", "name": "add_numbers", "args": {"a": 2, "b": "40"}}) + "\n").encode())
            fw.sendall(("! tool " + json.dumps({"id": "c2", "name": "spin", "args": {}}) + "\n").encode())
            fw.sendall(("! tool " + json.dumps({"id": "c3", "name": "fetch_test", "args": {}}) + "\n").encode())
            fw.sendall(("! tool " + json.dumps({"id": "c4", "name": "nope", "args": {}}) + "\n").encode())
        if len(results) >= 4 and any("timer fired" in l for l in logs):
            break
    alive = proc.poll() is None
    fw.close()
    try:
        proc.wait(5)
    except subprocess.TimeoutExpired:
        proc.kill()
    for l in logs:
        print("   log:", l[:160])
    check("the script ran and said hello", any("hello from js 2" in l for l in logs), logs)
    check("storage set/get", any("storage got 1" in l for l in logs), logs)
    check("a timer fired", any("timer fired" in l for l in logs))
    check("events reach the handlers", any("button event action" in l for l in logs) and any("media event spotify true Song Title" in l for l in logs), logs)
    check("subscriptions were asked for", set(subs) == {"button", "media"}, subs)
    check("tools were declared with a JSON schema", tools.get("add_numbers", {}).get("parameters", {}).get("required") == ["a", "b"] and "spin" in tools, tools)
    check("a tool call answers", results.get("c1") == "сумма 42", results)
    check("a runaway tool is interrupted and reported", "c2" in results and results["c2"].startswith("Ошибка"), results.get("c2"))
    check("http + play from a tool", results.get("c3") == "got 200 Тест-радио" and plays and plays[0]["title"] == "Тест-радио", (results.get("c3"), plays))
    check("an unknown tool answers an error", results.get("c4", "").startswith("Ошибка"), results.get("c4"))
    check("the plugin survived all of it", alive)
    print("ALL PASS" if OK else "SOME FAILED")
    sys.exit(0 if OK else 1)


main()
