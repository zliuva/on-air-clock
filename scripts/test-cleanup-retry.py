"""Exercise the native cleanup retry path against a local HTTP fixture."""
import http.server
import json
import pathlib
import subprocess
import threading

root = pathlib.Path(__file__).resolve().parents[1]
calls = []

class Handler(http.server.BaseHTTPRequestHandler):
    def do_DELETE(self):
        calls.append(self.path)
        payload = b'{}'
        self.send_response(503 if len(calls) < 3 else 200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)

    def log_message(self, *args):
        pass

server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
thread = threading.Thread(target=server.serve_forever, daemon=True)
thread.start()
config = root / '.build-cache' / 'cleanup-retry'
config.mkdir(parents=True, exist_ok=True)
(config / 'settings.json').write_text(json.dumps({
    'ClockUrl': f'http://127.0.0.1:{server.server_port}', 'Username': ''
}), encoding='utf-8')
try:
    result = subprocess.run([str(root / 'dist' / 'OnAirClock.exe'), '--clear',
                             '--data-dir', str(config)], capture_output=True, timeout=15)
    assert result.returncode == 0, result.stdout.decode()
    assert calls == ['/api/v1/notifications/on-air-clock'] * 3, calls
    print('PASS cleanup retries two HTTP failures and succeeds on the third named dismissal')
finally:
    server.shutdown()
    server.server_close()
