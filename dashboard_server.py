"""Live Arduino Uno serial-to-localhost classroom dashboard.

Requires pyserial. Standard-library HTTP server; no cloud or Internet dependency.
The Arduino IDE Serial Monitor must be closed while this process uses the port.
"""

from __future__ import annotations

import argparse
from collections import deque
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import mimetypes
import math
from pathlib import Path
import threading
import time
from urllib.parse import urlsplit

try:
    import serial
    from serial.tools import list_ports
except ImportError:
    raise SystemExit("Missing pyserial. Run: python -m pip install -r requirements.txt")

BAUD_RATE = 115200
WEB_DIR = Path(__file__).resolve().parent / "web"
DATA_COLUMNS = 12


def parse_data_line(line: str):
    """Parse one DATA CSV frame; never fill missing measurements with guesses."""
    fields = line.strip().split(",")
    if len(fields) != DATA_COLUMNS + 1 or fields[0] != "DATA":
        return None
    try:
        def nullable_float(v: str):
            if v in ("ERR", "NO_LOAD", ""):
                return None
            value = float(v)
            if not math.isfinite(value):
                raise ValueError("Non-finite reading")
            return value

        uptime = int(fields[1]); temp = nullable_float(fields[2]); humidity = nullable_float(fields[3])
        gas = int(fields[4]); ldr = int(fields[5])
        current = nullable_float(fields[6]); load_status = fields[7]
        if not 0 <= gas <= 1023 or not 0 <= ldr <= 1023:
            return None
        if load_status not in ("LOAD", "NO_LOAD", "ERROR"):
            return None
        if load_status == "LOAD" and current is None:
            return None
        if load_status != "LOAD" and current is not None:
            return None
        if fields[8] not in ("0", "1"):
            return None
        vibration = fields[8] == "1"
        ir = fields[9]
        if ir not in ("NO_OBJECT", "OBJECT_PASSED", "OBJECT_DETECTED"):
            return None
        events = int(fields[10])
        if events < 0 or uptime < 0:
            return None
        gas_status = fields[11]
        light = fields[12]
        if light not in ("BRIGHT", "DARK") or gas_status not in (
            "WARMUP", "ABOVE_DEMO_THRESHOLD", "BELOW_DEMO_THRESHOLD"
        ):
            return None
        return {
            "uptime_ms": uptime, "temperature_c": temp, "humidity_pct": humidity,
            "gas_raw": gas, "ldr_raw": ldr, "current_a": current,
            "load_status": load_status, "vibration": vibration,
            "ir_status": ir, "ir_events": events,
            "gas_status": gas_status, "light_status": light,
        }
    except (ValueError, OverflowError):
        return None


class State:
    def __init__(self):
        self.lock = threading.Lock()
        self.latest = None
        self.history = deque(maxlen=120)
        self.port_open = False
        self.info = "Waiting for Arduino USB serial connection"
        self.last_received_monotonic = None
        self.started_monotonic = time.monotonic()
        self.last_activity_monotonic = None
        self.serial_port = ""
        self.received_count = 0

    def update(self, reading):
        now = time.monotonic()
        with self.lock:
            # A transition into IR detection, or its recent release, is activity evidence,
            # never a headcount. SW-420 holds activity HIGH for 4 seconds.
            if reading["vibration"] or reading["ir_status"] in ("OBJECT_DETECTED", "OBJECT_PASSED"):
                self.last_activity_monotonic = now
            inactive_start = self.last_activity_monotonic or self.started_monotonic
            quiet_seconds = max(0, int(now - inactive_start))
            if reading["load_status"] == "ERROR":
                energy_status = "CURRENT SENSOR ERROR"
            elif reading["load_status"] == "NO_LOAD":
                energy_status = "NO MEASURABLE LOAD"
            elif quiet_seconds >= 20:
                energy_status = "POSSIBLE WASTE - VERIFY ROOM"
            else:
                energy_status = "LOAD ACTIVE"
            reading["evidence"] = (
                "ACTIVITY DETECTED" if reading["vibration"] or
                reading["ir_status"] in ("OBJECT_DETECTED", "OBJECT_PASSED")
                else "NO RECENT SENSOR ACTIVITY"
            )
            reading["energy_status"] = energy_status
            reading["quiet_seconds"] = quiet_seconds
            reading["received_at"] = datetime.now().astimezone().isoformat(timespec="seconds")
            self.latest = reading
            self.last_received_monotonic = now
            self.received_count += 1
            self.history.append(reading.copy())

    def snapshot(self, include_history=False):
        with self.lock:
            now = time.monotonic()
            age = None if self.last_received_monotonic is None else round(now - self.last_received_monotonic, 1)
            info = {
                "serial_connected": self.port_open, "fresh": self.port_open and age is not None and age < 8,
                "last_data_age_sec": age, "info": self.info, "port": self.serial_port,
                "received_count": self.received_count, "latest": self.latest,
                "note": "Activity heuristic only. AI occupancy model is not trained or deployed."
            }
            if include_history:
                info["history"] = list(self.history)
            return info


def port_choices():
    return list(list_ports.comports())


def serial_reader(state: State, port_name: str):
    while True:
        try:
            with serial.Serial(port_name, BAUD_RATE, timeout=1) as port:
                with state.lock:
                    state.port_open = True
                    state.info = f"Connected to {port_name}. Listening for DATA frames."
                print(f"\n[CONNECTED] {port_name} at {BAUD_RATE} baud", flush=True)
                print("[INFO] Opening USB serial may reset Uno; leave ACS712 CURRENT PATH unloaded for calibration.", flush=True)
                while True:
                    raw = port.readline()
                    if not raw:
                        continue
                    line = raw.decode("utf-8", errors="replace").strip()
                    if not line:
                        continue
                    # Mirror the readable Arduino reports into this terminal.
                    print(line, flush=True)
                    if line.startswith("DATA,"):
                        reading = parse_data_line(line)
                        if reading is None:
                            print("[WARN] Ignoring invalid DATA frame", flush=True)
                        else:
                            state.update(reading)
        except (serial.SerialException, OSError) as exc:
            with state.lock:
                state.port_open = False
                state.info = f"Serial unavailable: {exc}. Retrying..."
            print(f"[WAIT] Cannot use {port_name}: {exc}; retrying in 3s", flush=True)
            time.sleep(3)


def make_handler(state):
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            path = urlsplit(self.path).path
            if path == "/api/latest" or path == "/api/history":
                data = json.dumps(state.snapshot(include_history=(path == "/api/history")), allow_nan=False).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers()
                self.wfile.write(data)
                return
            allowed = {"/": "index.html", "/index.html": "index.html", "/style.css": "style.css", "/app.js": "app.js"}
            if path not in allowed:
                self.send_error(404)
                return
            filepath = WEB_DIR / allowed[path]
            content = filepath.read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", mimetypes.guess_type(str(filepath))[0] or "text/plain")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(content)))
            self.end_headers()
            self.wfile.write(content)

        def log_message(self, fmt, *args):
            pass  # Avoid filling console with browser polling messages.

    return Handler


def main():
    parser = argparse.ArgumentParser(description="Arduino Uno live classroom dashboard")
    parser.add_argument("--port", help="Arduino COM port, e.g. COM6; auto if exactly one serial port")
    parser.add_argument("--http-port", type=int, default=8000, help="Dashboard TCP port (default 8000)")
    parser.add_argument("--list", action="store_true", help="List available serial ports and exit")
    args = parser.parse_args()
    choices = port_choices()
    if args.list:
        if not choices:
            print("No serial ports detected. Check USB cable, device drivers and Arduino.")
        for p in choices:
            print(f"{p.device}  -  {p.description}")
        return
    port = args.port
    if not port:
        if len(choices) != 1:
            print("Select your Arduino port. Run: python dashboard_server.py --list")
            print("Then: python dashboard_server.py --port COM6   (replace COM6)")
            for p in choices:
                print(f"  {p.device} - {p.description}")
            raise SystemExit(2)
        port = choices[0].device
    state = State()
    state.serial_port = port
    threading.Thread(target=serial_reader, args=(state, port), daemon=True).start()
    server = ThreadingHTTPServer(("127.0.0.1", args.http_port), make_handler(state))
    print(f"[DASHBOARD] Open http://127.0.0.1:{args.http_port} in your browser")
    print("[IMPORTANT] Close Arduino IDE Serial Monitor/Plotter while dashboard uses the COM port.")
    print("[INFO] Human-readable serial reports are mirrored in THIS terminal.")
    try:
        server.serve_forever(poll_interval=0.3)
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
