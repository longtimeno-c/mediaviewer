#!/usr/bin/env python3
# Copyright (C) 2026 longtimeno-c
# SPDX-License-Identifier: GPL-3.0-or-later
"""The Tor spike's harness (issue #472, option 11; docs/plans/sharepoint-tor-spike.md).

Measures C tor on this machine, against the LIVE Tor network: binary size, time to bootstrap,
time from ADD_ONION to a first connection to that onion, and throughput through it. Never run by
CI or tests; refuses without --live-tor. Writes only a temporary data directory, removed after.
Prints one JSON line per run. Standard library only.
"""
import argparse
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time


def free_port():
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def binary_size(path):
    total = os.path.getsize(path)
    libs = []
    try:
        if sys.platform == "darwin":
            out = subprocess.run(["otool", "-L", path], capture_output=True, text=True).stdout
            libs = [l.split()[0] for l in out.splitlines()[1:] if l.strip() and not l.strip().startswith("/usr/lib")
                    and not l.strip().startswith("/System")]
        elif sys.platform.startswith("linux"):
            out = subprocess.run(["ldd", path], capture_output=True, text=True).stdout
            libs = [l.split("=>")[1].split()[0] for l in out.splitlines() if "=>" in l and "/" in l]
    except OSError:
        pass
    for lib in libs:
        if os.path.exists(lib):
            total += os.path.getsize(lib)
    return os.path.getsize(path), total


class control:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.f = self.s.makefile("rwb")

    def cmd(self, line):
        self.f.write((line + "\r\n").encode())
        self.f.flush()
        lines = []
        while True:
            l = self.f.readline().decode().rstrip("\r\n")
            lines.append(l)
            if len(l) >= 4 and l[3] == " ":
                return lines


def socks_connect(socks_port, host, port, timeout=120):
    s = socket.create_connection(("127.0.0.1", socks_port), timeout=timeout)
    s.sendall(b"\x05\x01\x00")
    if s.recv(2) != b"\x05\x00":
        raise OSError("socks greeting")
    h = host.encode()
    s.sendall(b"\x05\x01\x00\x03" + bytes([len(h)]) + h + struct.pack(">H", port))
    r = s.recv(10)
    if len(r) < 2 or r[1] != 0:
        raise OSError("socks connect failed: %r" % r[:2])
    return s


def run_once(tor, mb, bootstrap_timeout):
    data = tempfile.mkdtemp(prefix="mv-tor-spike-")
    ctl, socks = free_port(), free_port()
    t0 = time.monotonic()
    cookie = os.path.join(data, "control_auth_cookie")
    # Cookie authentication: only this user can read the cookie, so nothing
    # else on the machine can drive this tor's control port.
    p = subprocess.Popen([tor, "--DataDirectory", data, "--ControlPort", "127.0.0.1:%d" % ctl,
                          "--SocksPort", "127.0.0.1:%d" % socks, "--CookieAuthentication", "1",
                          "--CookieAuthFile", cookie, "--Log", "notice stdout"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    result = {"case": "tor_spike"}
    booted = threading.Event()

    def read_log():
        # Drained to the end, so tor never blocks on a full pipe.
        for line in p.stdout:
            if "Bootstrapped 100%" in line:
                booted.set()

    reader = threading.Thread(target=read_log, daemon=True)
    reader.start()
    try:
        if not booted.wait(bootstrap_timeout):
            raise OSError("tor did not bootstrap in %d s" % bootstrap_timeout)
        boot = time.monotonic() - t0
        result["bootstrap_s"] = round(boot, 2)
        # A sink the onion forwards to.
        sink = socket.socket()
        sink.bind(("127.0.0.1", 0))
        sink.listen(1)
        got = {"bytes": 0}

        def drain():
            c, _ = sink.accept()
            while True:
                b = c.recv(1 << 16)
                if not b:
                    break
                got["bytes"] += len(b)
            c.close()

        c = control(ctl)
        with open(cookie, "rb") as f:
            r = c.cmd("AUTHENTICATE " + f.read().hex())
        if not r or not r[-1].startswith("250"):
            raise OSError("control port refused the cookie")
        t1 = time.monotonic()
        r = c.cmd("ADD_ONION NEW:ED25519-V3 Flags=DiscardPK Port=80,127.0.0.1:%d" % sink.getsockname()[1])
        onion = next(l.split("=", 1)[1] for l in r if "ServiceID=" in l) + ".onion"
        threading.Thread(target=drain, daemon=True).start()
        s = None
        while s is None and time.monotonic() - t1 < 300:
            try:
                s = socks_connect(socks, onion, 80)
            except OSError:
                time.sleep(2)
        if s is None:
            raise OSError("onion not reachable in 300 s")
        result["onion_reachable_s"] = round(time.monotonic() - t1, 2)
        chunk = os.urandom(1 << 16)
        n = mb * (1 << 20)
        t2 = time.monotonic()
        sent = 0
        while sent < n:
            s.sendall(chunk)
            sent += len(chunk)
        s.shutdown(socket.SHUT_WR)
        while got["bytes"] < n and time.monotonic() - t2 < 3600:
            time.sleep(0.1)
        dt = time.monotonic() - t2
        result["mb"] = mb
        result["throughput_mb_s"] = round(got["bytes"] / dt / 1e6, 2)
        s.close()
    except (OSError, StopIteration) as e:
        result["error"] = str(e)
    finally:
        p.terminate()
        try:
            p.wait(10)
        except subprocess.TimeoutExpired:
            p.kill()
        reader.join(5)
        shutil.rmtree(data, ignore_errors=True)
    return result


def main():
    a = argparse.ArgumentParser(description=__doc__)
    a.add_argument("--tor", required=True, help="path to a C tor binary")
    a.add_argument("--mb", type=int, default=64)
    a.add_argument("--runs", type=int, default=3)
    a.add_argument("--bootstrap-timeout", type=int, default=300, help="seconds to wait for bootstrap 100%%")
    a.add_argument("--live-tor", action="store_true", help="required: this joins the live Tor network")
    o = a.parse_args()
    if not o.live_tor:
        print("refusing: this measures against the live Tor network; pass --live-tor", file=sys.stderr)
        return 2
    exe, with_libs = binary_size(o.tor)
    print(json.dumps({"case": "tor_size", "binary_bytes": exe, "with_libraries_bytes": with_libs}))
    for _ in range(o.runs):
        print(json.dumps(run_once(o.tor, o.mb, o.bootstrap_timeout)), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
