import os
import http.server
import socketserver

PORT = int(os.environ.get("PORT", 8080))

# Serve files from the repo root (index.html, style.css, sources.html, logo.png)
SERVE_DIR = os.path.dirname(os.path.abspath(__file__))


class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=SERVE_DIR, **kwargs)

    def log_message(self, fmt, *args):
        print(fmt % args)


with socketserver.TCPServer(("", PORT), Handler) as httpd:
    print(f"Plotter static site serving on port {PORT}")
    httpd.serve_forever()
