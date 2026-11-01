"""Tests of the Lua mode of lostos-js on this computer, against a stand-in for the firmware (no sandbox).
Usage: python3 test_lua.py <lostos-js built for this computer>   (build.sh <out dir> x86_64-linux-musl)"""
import os
import socket
import subprocess
import sys
import time

runner = sys.argv[1]
bad = 0


def check(name, cond, extra=""):
    global bad
    print(("ok   " if cond else "FAIL ") + name + ("" if cond else "  " + str(extra)))
    bad += not cond


def run(script, device="model=bergamot ring=6 logo=0 clock=0 microphone=1 plugin=demo.x", seconds=3.0, events=()):
    """Starts the runtime with a script, answers its requests, returns the list of (method, args)."""
    fw, pl = socket.socketpair()
    os.set_inheritable(pl.fileno(), True)
    proc = subprocess.Popen([runner], preexec_fn=lambda: os.dup2(pl.fileno(), 3), close_fds=False)
    pl.close()
    src = script.encode()
    fw.sendall(b"! script %d\n" % len(src) + src)
    seen, buf, end = [], b"", time.time() + seconds
    fw.settimeout(0.2)
    while time.time() < end:
        try:
            chunk = fw.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
        except socket.timeout:
            continue
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            n, _, rest = line.decode().partition(" ")
            method, _, args = rest.partition(" ")
            seen.append((method, args))
            if n != "0":
                fw.sendall((n + " ok " + (device if method == "device" else "") + "\n").encode())
            if method == "subscribe":
                for e in events:
                    fw.sendall(("! button " + e + "\n").encode())
    proc.kill()
    proc.wait()
    return seen


r = run('--lua\nprint("hello", 1, true)\n')
check("print goes to the log", ("log", "hello 1 true") in r and ("log", "started") in r, r)

r = run('--lua\nprint(device.model, device.has_ring, device.ring_count, device.has_clock, device.HasRing, #device.features)\n')
check("device table", ("log", "bergamot true 6 false true 5") in r, r)

r = run('--lua\nalice.led.set.ring({"#ff0000", "#0000ff"})\nalice.sleep(50)\nalice.led.off.ring()\n')
check("led: mean colour, then off", ("led", "127 0 127 30") in r and ("led", "0 0 0 1") in r, r)

r = run('--lua\nif alice.clock.is_available() then alice.clock.time("12:34", 170) end\nprint(alice.is_available("bluetooth.scan"), alice.audio.is_available())\n',
        device="model=plum ring=3 logo=0 clock=1 microphone=1")
check("clock shows the time; unsupported things say so", ("display", "30 12:34") in r and ("log", "false true") in r, r)

r = run('--lua\nlocal ok, why = pcall(register.customweb, "../x.js")\nprint(ok, why)\nlocal ok2 = pcall(function() alice.led.set.ring({"red"}) end)\nprint(ok2)\n')
check("a call that cannot be done raises an error", any(m == "log" and a.startswith("false ") and ".html" in a for m, a in r) and ("log", "false") in r, r)

r = run('--lua\nprint(io, os, require, dofile)\n')
check("no files, no os, no require", ("log", "nil nil nil nil") in r, r)

r = run('--lua\nlocal t = {}\nfor i = 1, 1e7 do t[i] = ("x"):rep(100) .. i end\n', seconds=4)
check("memory is capped, the plugin reports the error", any(m == "log" and a.startswith("error:") for m, a in r), r)

r = run('--lua\nthis is not lua\n')
check("a syntax error is reported", any(m == "log" and a.startswith("error:") for m, a in r), r)

r = run('--lua\nlostos.storage.set("k", "v")\nprint(lostos.storage.get("k") == "" and "empty" or "value")\n')
check("lostos.storage goes through the firmware", ("set", "k v") in r and ("get", "k") in r, r)

r = run('''--lua
alice.volume.set(55)
alice.audio.play("sounds/a.wav")
local ok = pcall(alice.volume.set, 300)
print(ok)
''')
check("volume.set and audio.play go to the firmware", ("media", "volume 55") in r and ("playfile", "sounds/a.wav") in r and ("log", "false") in r, r)

r = run('''--lua
local e = alice.input.next_event(2000)
print(e and e.action or "none")
local e2 = alice.input.next_event(300)
print(e2 and e2.action or "none")
''', events=("action",))
check("input.next_event returns a key press, then nil on timeout", ("subscribe", "button") in r and ("log", "action") in r and ("log", "none") in r, r)

r = run("--lua" + chr(10) + 'print(register.customweb("web/index.html"))' + chr(10))
check("register.customweb gives the address of the page on the server", ("log", "/luna/web/demo.x/web/index.html") in r, r)

r = run("--lua" + chr(10) + '__LUNA = {allow = {"api.example.com", "*.cdn.org"}, insecure = false}' + chr(10) +
        'print(pcall(alice.http.get, "https://evil.com/x"))' + chr(10) + 'print(pcall(alice.http.get, "http://api.example.com/x"))' + chr(10) +
        'print(pcall(alice.http.get, "https://a.cdn.org/x"))' + chr(10))
check("http: only hosts of [network].allow and https", any(m == "log" and a.startswith("false") and "not in [network].allow" in a for m, a in r) and any(m == "log" and a.startswith("false") and "https" in a for m, a in r)
      and any(m == "http" and "a.cdn.org" in a for m, a in r), r)

r = run("--lua" + chr(10) + 'alice.clock.segments({A1 = 255, G4 = 128})' + chr(10) + 'alice.clock.off()' + chr(10))
want = "00" * 28
lv = ["00"] * 28
lv[0], lv[27] = "ff", "80"
check("clock.segments sends the 28 levels (digits 1..4, segments A..G)", ("segments", "60000 " + "".join(lv)) in r and ("segments", "0") in r, r)

print("%d failed" % bad if bad else "all passed")
sys.exit(1 if bad else 0)
