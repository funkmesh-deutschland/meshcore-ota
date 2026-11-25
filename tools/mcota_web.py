import base64
import hmac
import json
import os
import sys
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def page_path():
    base = getattr(sys, "_MEIPASS", None)
    if base:
        return os.path.join(base, "web", "index.html")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "web", "index.html")


class Web:
    def __init__(self, host, port, password=None):
        self.host = host
        self.port = port
        self.password = password
        self.state = {}
        self.handlers = {}
        self.server = None
        self.page = open(page_path(), "rb").read()

    def allowed(self, request):
        if not self.password:
            return True
        header = request.headers.get("Authorization", "")
        if not header.startswith("Basic "):
            return False
        try:
            given = base64.b64decode(header[6:]).decode().partition(":")[2]
        except ValueError:
            return False
        return hmac.compare_digest(given.encode(), self.password.encode())

    def start(self):
        web = self

        class Handler(BaseHTTPRequestHandler):
            def reply(self, code, body, kind="application/json"):
                self.send_response(code)
                self.send_header("Content-Type", kind)
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                if code == 401:
                    self.send_header("WWW-Authenticate", 'Basic realm="meshcore-ota"')
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if not web.allowed(self):
                    self.reply(401, b"{}")
                elif self.path in ("/", "/index.html"):
                    self.reply(200, web.page, "text/html; charset=utf-8")
                elif self.path == "/api/state":
                    self.reply(200, json.dumps(web.state).encode())
                else:
                    self.reply(404, b"{}")

            def do_POST(self):
                name = self.path[5:] if self.path.startswith("/api/") else ""
                if not web.allowed(self):
                    self.reply(401, b"{}")
                elif name not in web.handlers or self.headers.get("Content-Type", "").split(";")[0] != "application/json":
                    self.reply(404, b"{}")
                else:
                    try:
                        size = int(self.headers.get("Content-Length", "0"))
                        payload = json.loads(self.rfile.read(size) or b"{}") if size <= 65536 else None
                        if not isinstance(payload, dict):
                            raise ValueError("invalid request")
                        self.reply(200, json.dumps(web.handlers[name](payload) or {}).encode())
                    except (ValueError, KeyError, SystemExit) as error:
                        self.reply(400, json.dumps({"error": str(error)}).encode())

            def log_message(self, *args):
                pass

        self.server = ThreadingHTTPServer((self.host, self.port), Handler)
        self.server.daemon_threads = True
        self.port = self.server.server_address[1]
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def stop(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.server = None

    def address(self):
        host = "localhost" if self.host in ("127.0.0.1", "localhost") else (self.host if self.host != "0.0.0.0" else "<address of this computer>")
        return f"http://{host}:{self.port}/"
