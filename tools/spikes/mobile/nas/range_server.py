#!/usr/bin/env python3
# M1 spikes S2 and S4 (throwaway). Standard library only.
#
#   range_server.py serve FILE [--port 47615]     one file, HTTP byte ranges, behind a random token
#   range_server.py probe URL [--reads 200]       small-range latency and throughput from elsewhere
#
# NOT Driftwire: plain http, so it measures the PATH through the forward (RTT, throughput, what a
# range read costs), not the link's crypto or framing. `driftwire` has no range read yet (M2b).
# Serve a test clip you would not mind a stranger seeing, and close the forward afterwards.
import argparse
import http.client
import os
import random
import re
import secrets
import socket
import statistics
import sys
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def serve(args):
    path = os.path.abspath(args.file)
    size = os.path.getsize(path)
    token = secrets.token_urlsafe(18)
    name = urllib.parse.quote(os.path.basename(path))
    expected = f"/{token}/{name}"

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *_):
            pass

        def do_GET(self):
            if self.path != expected:
                self.send_error(404)
                return
            first, last, status = 0, size - 1, 200
            header = self.headers.get("Range")
            if header:
                match = re.fullmatch(r"bytes=(\d+)-(\d*)", header.strip())
                if not match or int(match.group(1)) >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", f"bytes */{size}")
                    self.send_header("Content-Length", "0")
                    self.end_headers()
                    return
                first = int(match.group(1))
                last = min(int(match.group(2)), size - 1) if match.group(2) else size - 1
                status = 206
            self.send_response(status)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "video/mp4")
            self.send_header("Content-Length", str(last - first + 1))
            if status == 206:
                self.send_header("Content-Range", f"bytes {first}-{last}/{size}")
            self.end_headers()
            try:
                with open(path, "rb") as f:
                    f.seek(first)
                    left = last - first + 1
                    while left > 0:
                        chunk = f.read(min(1 << 20, left))
                        if not chunk:
                            break
                        self.wfile.write(chunk)
                        left -= len(chunk)
            except (BrokenPipeError, ConnectionResetError):
                self.close_connection = True

    class Server(ThreadingHTTPServer):
        address_family = socket.AF_INET6
        daemon_threads = True

        def server_bind(self):
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
            super().server_bind()

    server = Server(("::", args.port), Handler)
    print(f"serving {size} bytes on port {args.port}, IPv4 and IPv6", flush=True)
    print(f"path: {expected}", flush=True)
    print(f"URL:  http://<address or name>:{args.port}{expected}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


def probe(args):
    url = urllib.parse.urlsplit(args.url)
    connect_start = time.perf_counter()
    conn = http.client.HTTPConnection(url.hostname, url.port or 80, timeout=30)
    conn.connect()
    connect_ms = (time.perf_counter() - connect_start) * 1000
    peer = conn.sock.getpeername()

    def fetch(first, count):
        conn.request("GET", url.path, headers={"Range": f"bytes={first}-{first + count - 1}"})
        response = conn.getresponse()
        data = response.read()
        if response.status != 206:
            sys.exit(f"expected 206, got {response.status}")
        return data, response.getheader("Content-Range")

    _, content_range = fetch(0, 1)
    size = int(content_range.rsplit("/", 1)[1])

    times = []
    for _ in range(args.reads):
        first = random.randrange(0, max(1, size - args.read_size))
        start = time.perf_counter()
        fetch(first, args.read_size)
        times.append((time.perf_counter() - start) * 1000)
    times.sort()

    count = min(size, args.bulk_mb << 20)
    start = time.perf_counter()
    data, _ = fetch(0, count)
    seconds = time.perf_counter() - start

    print(f"peer {peer[0]} port {peer[1]}  (an address in 64:ff9b::/96 is NAT64)")
    print(f"connect_ms {connect_ms:.1f}")
    print(f"file_bytes {size}")
    print(f"range_{args.read_size}B_reads {len(times)}")
    print(f"range_p50_ms {statistics.median(times):.1f}")
    print(f"range_p95_ms {times[int(len(times) * 0.95) - 1]:.1f}")
    print(f"range_max_ms {times[-1]:.1f}")
    print(f"bulk_mbit_s {len(data) * 8 / seconds / 1e6:.1f}  ({len(data)} bytes)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    s = commands.add_parser("serve")
    s.add_argument("file")
    s.add_argument("--port", type=int, default=47615)
    s.set_defaults(run=serve)
    p = commands.add_parser("probe")
    p.add_argument("url")
    p.add_argument("--reads", type=int, default=200)
    p.add_argument("--read-size", type=int, default=64 * 1024)
    p.add_argument("--bulk-mb", type=int, default=64)
    p.set_defaults(run=probe)
    args = parser.parse_args()
    args.run(args)


if __name__ == "__main__":
    main()
