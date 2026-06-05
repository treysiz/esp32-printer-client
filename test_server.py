#!/usr/bin/env python3
"""
Mock backend for the ESP32 "WiFi pull-order" HTTP printer client.

Reproduces the real backend's printer-api contract so you can test the
firmware end-to-end WITHOUT the production server:

    POST /printer-api/heartbeat         -> {"success":true,"printerId","name"}
    GET  /printer-api/jobs              -> {"success":true,"data":[ job, ... ]}
    POST /printer-api/jobs/<id>/done    -> {"success":true}
    POST /printer-api/jobs/<id>/failed  -> {"success":true}

Auth: every request must carry  Authorization: Bearer <API_TOKEN>
      (missing -> 401, wrong -> 401).

`content` is encoded EXACTLY like the real backend: raw ESC/POS bytes are
latin1-decoded to a string, then placed into JSON (served as UTF-8). That is
what exercises the firmware's byte-exact latin1/UTF-8 decoder. With Pillow
installed the mock renders a real GS v 0 raster receipt (Chinese included);
otherwise it falls back to a plain ASCII ESC/POS text receipt.

CJK strings below are written as \\uXXXX escapes so this source file stays
pure ASCII (some editors truncate files at raw multibyte characters).

Usage:
    python3 test_server.py                      # 0.0.0.0:3000, token "testtoken"
    python3 test_server.py --port 3000 --token testtoken

Firmware web UI:
    Backend URL = http://<this-machine-ip>:3000
    API Token   = testtoken
Press Enter in this console to queue a new job for the printer to pull.
"""
import argparse
import json
import threading
import uuid
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

API_TOKEN = "testtoken"

JOBS = {}            # id -> job dict (status: pending|printed|failed)
LOCK = threading.Lock()


def escpos_text_receipt(order_number, lines):
    # Plain ESC/POS text receipt (ASCII only; fallback when Pillow is absent).
    out = bytearray()
    out += b"\x1b\x40"                       # ESC @ init
    out += b"\x1b\x61\x01"                   # center
    out += b"*** TEST ORDER ***\n"
    out += ("%s\n" % order_number).encode("ascii", "replace")
    out += b"\x1b\x61\x00"                   # left
    out += b"--------------------------------\n"
    for ln in lines:
        out += (ln + "\n").encode("ascii", "replace")
    out += b"--------------------------------\nThank you!\n\n\n\n"
    out += b"\x1d\x56\x00"                   # GS V 0 full cut
    return bytes(out)


def escpos_raster_receipt(order_number, lines, width=384):
    # Render a real raster receipt (GS v 0) from text via Pillow so Chinese
    # prints correctly, like the production backend. Returns None if Pillow or
    # a CJK font is unavailable.
    try:
        from PIL import Image, ImageDraw, ImageFont
    except Exception:
        return None

    title = "*** TEST 测试订单 ***"   # measure-test-order
    thanks = "谢谢惠顾!"               # thank you
    text = title + "\n%s\n" % order_number
    text += "-" * 24 + "\n" + "\n".join(lines) + "\n" + "-" * 24 + "\n" + thanks + "\n"

    font = None
    for path in ("/usr/share/fonts/truetype/wqy/wqy-zenhei.ttc",
                 "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                 "/System/Library/Fonts/PingFang.ttc",
                 "C:/Windows/Fonts/msyh.ttc"):
        try:
            font = ImageFont.truetype(path, 22)
            break
        except Exception:
            continue
    if font is None:
        font = ImageFont.load_default()

    rows = text.split("\n")
    line_h = 26
    height = line_h * len(rows) + 20
    img = Image.new("1", (width, height), 1)   # 1 = white
    draw = ImageDraw.Draw(img)
    y = 10
    for r in rows:
        draw.text((4, y), r, font=font, fill=0)  # 0 = black
        y += line_h

    bytes_per_row = (width + 7) // 8
    raster = bytearray()
    px = img.load()
    for yy in range(height):
        for xb in range(bytes_per_row):
            b = 0
            for bit in range(8):
                x = xb * 8 + bit
                if x < width and px[x, yy] == 0:
                    b |= (0x80 >> bit)
            raster.append(b)

    out = bytearray()
    out += b"\x1b\x40"                          # init
    xl = bytes_per_row & 0xff
    xh = (bytes_per_row >> 8) & 0xff
    yl = height & 0xff
    yh = (height >> 8) & 0xff
    out += bytes([0x1d, 0x76, 0x30, 0x00, xl, xh, yl, yh])  # GS v 0
    out += raster
    out += b"\n\n\x1d\x56\x00"                  # feed + cut
    return bytes(out)


def make_job(order_number, copies=1):
    menu = ["Kung Pao 宫保鸡丁 x1",
            "Mapo Tofu 麻婆豆腐 x2",
            "Rice 米饭 x3"]
    payload = escpos_raster_receipt(order_number, menu)
    kind = "raster(CN)"
    if payload is None:
        payload = escpos_text_receipt(order_number, ["Item A x1", "Item B x2"])
        kind = "text(ascii fallback)"
    jid = str(uuid.uuid4())
    job = {
        "id": jid,
        "printerId": "mock-printer",
        "storeId": "mock-store",
        "orderNumber": order_number,
        "content": payload.decode("latin1"),   # backend encoding
        "copies": copies,
        "status": "pending",
        "createdAt": "2026-06-04T21:44:00.000Z",
        "printedAt": None,
    }
    with LOCK:
        JOBS[jid] = job
    print("[queued] order=%s id=%s %dB %s copies=%d"
          % (order_number, jid, len(payload), kind, copies))
    return job


class Handler(BaseHTTPRequestHandler):
    def _auth_ok(self):
        h = self.headers.get("Authorization", "")
        if not h.startswith("Bearer "):
            self._json(401, {"success": False, "error": "Missing Bearer token"})
            return False
        if h[len("Bearer "):] != API_TOKEN:
            self._json(401, {"success": False, "error": "Invalid or inactive token"})
            return False
        return True

    def _json(self, code, obj):
        body = json.dumps(obj).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path != "/printer-api/jobs":
            return self._json(404, {"success": False, "error": "not found"})
        if not self._auth_ok():
            return
        with LOCK:
            pending = [j for j in JOBS.values() if j["status"] == "pending"]
        self._json(200, {"success": True, "data": pending})

    def do_POST(self):
        if not self._auth_ok():
            return
        p = self.path
        if p == "/printer-api/heartbeat":
            print("[heartbeat]")
            return self._json(200, {"success": True,
                                    "printerId": "mock-printer", "name": "Mock"})
        if p.startswith("/printer-api/jobs/") and (p.endswith("/done") or p.endswith("/failed")):
            jid = p.split("/")[3]
            verb = "done" if p.endswith("/done") else "failed"
            with LOCK:
                job = JOBS.get(jid)
                if not job:
                    return self._json(404, {"success": False, "error": "not found"})
                job["status"] = "printed" if verb == "done" else "failed"
            print("[%s] id=%s -> status now %s" % (verb, jid, job["status"]))
            return self._json(200, {"success": True})
        self._json(404, {"success": False, "error": "not found"})


def console():
    n = 1
    while True:
        try:
            cmd = input("Press Enter to queue a test order (c<N>=N copies, q=quit): ").strip()
        except EOFError:
            return
        if cmd.lower() == "q":
            import os
            os._exit(0)
        copies = 1
        if cmd.lower().startswith("c") and cmd[1:].isdigit():
            copies = int(cmd[1:])
        make_job("#T%d" % n, copies=copies)
        n += 1


def main():
    global API_TOKEN
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=3000)
    ap.add_argument("--token", default=API_TOKEN)
    args = ap.parse_args()
    API_TOKEN = args.token

    srv = ThreadingHTTPServer(("0.0.0.0", args.port), Handler)
    print("Mock backend on http://0.0.0.0:%d  (Bearer token: %s)" % (args.port, API_TOKEN))
    threading.Thread(target=console, daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("\nStopped.")


if __name__ == "__main__":
    main()
