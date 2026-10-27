import pathlib
import subprocess
import tempfile

script = pathlib.Path(__file__).with_name('start.sh').read_text()
with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    for name in ('ed25519', 'ecdsa', 'rsa'):
        (root / ('host_' + name)).write_text('test-host-key')
    daemon = root / 'dropbear'
    daemon.write_text('#!/bin/sh\nprintf "%s\\n" "$@"\n')
    daemon.chmod(0o700)
    pidof = root / 'pidof'
    pidof.write_text('#!/bin/sh\nexit 1\n')
    pidof.chmod(0o700)
    result = subprocess.run(['sh'], input=script.replace('D=/data/dropbear', 'D=' + folder),
                            env={'PATH': folder + ':/usr/bin:/bin'}, capture_output=True, text=True, check=True)
    args = result.stdout.splitlines()
    assert '-B' not in args, args
    assert '-s' in args, args
    assert args[args.index('-p') + 1] == '22', args
    (root / 'stale.pid').write_text('99999999\n')
    stale = script.replace('D=/data/dropbear', 'D=' + folder).replace('/run/dropbear.pid', str(root / 'stale.pid'))
    result = subprocess.run(['sh'], input=stale, env={'PATH': folder + ':/usr/bin:/bin'}, capture_output=True, text=True, check=True)
    assert '-s' in result.stdout.splitlines()
print('ALL PASS: SSH allows only public-key authentication')

import os
import threading

with tempfile.TemporaryDirectory() as folder:
    root = pathlib.Path(folder)
    source = root / 'daemon.c'
    source.write_text('''#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
int main(int argc, char **argv) {
  if (getenv("TEST_KEEP_ALIVE")) { for (;;) pause(); }
  for (int i=1; i<argc; i++) puts(argv[i]);
  return 0;
}
''')
    executable = root / 'dropbearmulti'
    subprocess.run(['cc', str(source), '-o', str(executable)], check=True)
    (root / 'dropbear').symlink_to(executable)
    for name in ('ed25519', 'ecdsa', 'rsa'):
        (root / ('host_' + name)).write_text('test-host-key')
    process = subprocess.Popen([str(executable), '-B'], env=dict(os.environ, TEST_KEEP_ALIVE='1'))
    reaper = threading.Thread(target=process.wait)
    reaper.start()
    pidfile = root / 'daemon.pid'
    pidfile.write_text(str(process.pid) + '\n')
    try:
        adapted = script.replace('D=/data/dropbear', 'D=' + folder).replace('/run/dropbear.pid', str(pidfile))
        result = subprocess.run(['sh'], input=adapted, capture_output=True, text=True, timeout=10, check=True)
        assert '-s' in result.stdout.splitlines()
        assert '-B' not in result.stdout.splitlines()
        reaper.join(2)
        assert process.returncode is not None
    finally:
        if process.poll() is None:
            process.kill()
        reaper.join(2)
print('ALL PASS: an existing password-enabled lostOS daemon is replaced')
