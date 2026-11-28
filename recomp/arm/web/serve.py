#!/usr/bin/env python3
"""Serve an ANDWASM web build with the cross-origin isolation headers threads need.
Usage: python3 recomp/arm/web/serve.py <dir> [port]"""
import http.server, os, sys


class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


if __name__ == "__main__":
    os.chdir(sys.argv[1])
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8655
    http.server.ThreadingHTTPServer(("127.0.0.1", port), H).serve_forever()
