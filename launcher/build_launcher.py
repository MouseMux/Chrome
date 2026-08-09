"""Builds launcher.exe.

Reads the Windows SDK include/lib paths out of the Chromium build's
environment.x64 block so the launcher compiles with the same toolchain as
Chrome, without needing a Visual Studio developer prompt.
"""
import os
import subprocess
import sys

SRC = r"O:/mousemux-builds/Chrome/chrome/src"
CLANG = SRC + "/third_party/llvm-build/Release+Asserts/bin/clang-cl.exe"
ENVFILE = SRC + "/out/Release/environment.x64"

env = dict(os.environ)
with open(ENVFILE, "rb") as f:
    blob = f.read()
for entry in blob.split(b"\0"):
    if b"=" in entry:
        k, v = entry.split(b"=", 1)
        env[k.decode("mbcs")] = v.decode("mbcs")

cmd = [
    CLANG, "launcher.c", "/TC", "/MT", "/O2", "/W4", "/WX",
    "/Fe:launcher.exe", "/Fo:launcher.obj",
    "/link", "ws2_32.lib", "user32.lib", "kernel32.lib",
    "/subsystem:windows", "/entry:mainCRTStartup",
]
sys.exit(subprocess.call(cmd, env=env, cwd=os.path.dirname(os.path.abspath(__file__))))
