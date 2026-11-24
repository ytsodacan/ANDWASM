#!/usr/bin/env python3
"""Local static server with the cross-origin isolation headers threaded wasm needs.
Usage: python3 web/serve.py <folder with index.html and the ANDWASM output> [port=8000]"""
import http.server, sys, functools, os
folder = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
port = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
class H(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-cache")
        super().end_headers()
    def log_message(self, *a): pass
print(f"serving {folder} at http://localhost:{port}/")
http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(H, directory=folder)).serve_forever()
