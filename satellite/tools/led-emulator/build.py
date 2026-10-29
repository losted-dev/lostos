"""Builds tools/led_sim.cpp for this computer (zig c++, g++ or clang++, the first one that exists) and writes frames.js next to index.html.
Run from anywhere:  python tools/led-emulator/build.py        then open tools/led-emulator/index.html in a browser."""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SAT = os.path.dirname(os.path.dirname(HERE))
SRC = [os.path.join(SAT, "tools", "led_sim.cpp"), os.path.join(SAT, "src", "hw", "led_fx.cpp"), os.path.join(SAT, "src", "hw", "led_matrix.cpp")]


def compiler():
    for cand in (["zig", "c++"], ["g++"], ["clang++"]):
        if shutil.which(cand[0]):
            return cand
    sys.exit("no C++ compiler found: install zig, g++ or clang++")


def main():
    exe = os.path.join(tempfile.mkdtemp(), "led_sim" + (".exe" if os.name == "nt" else ""))
    cmd = compiler() + ["-std=c++20", "-O1", "-I", os.path.join(SAT, "src")] + SRC + ["-o", exe]
    print(" ".join(cmd))
    subprocess.run(cmd, check=True)
    out = os.path.join(HERE, "frames.js")
    subprocess.run([exe, out], check=True)
    print("written", out, round(os.path.getsize(out) / 1024), "KB")


if __name__ == "__main__":
    main()
