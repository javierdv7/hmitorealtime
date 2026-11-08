#!/usr/bin/env python3
"""Edge Rules HMI — tiny web HMI served from the CM4.

Subscribes to the device topics and streams a merged state to browsers via Server-Sent Events.
Stdlib HTTP server + paho-mqtt only; the page has no external assets (works with no internet).
"""
from __future__ import annotations

import json
import os
import threading
import time
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import sys

import paho.mqtt.client as mqtt

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "agent"))
import llm as llmlib  # noqa: E402
import edge_agent as agentlib  # noqa: E402

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
DEVICE_ID = os.environ.get("DEVICE_ID", "edge01")
DEVICE_HTTP = os.environ.get("DEVICE_HTTP", "http://192.168.1.70")
HTTP_PORT = int(os.environ.get("HMI_PORT", "8090"))
INDEX = (Path(__file__).parent / "index.html").read_bytes()
CLEAR_PATH = Path(__file__).resolve().parent.parent / "config" / "cleared_seq.json"

T = {k: f"edge/{DEVICE_ID}/{k}" for k in ("telemetry", "status")}
T["state"] = f"edge/{DEVICE_ID}/rules/state"
T["set"] = f"edge/{DEVICE_ID}/rules/set"
T["log"] = f"edge/{DEVICE_ID}/log"

lock = threading.Lock()
model: dict = {"device": DEVICE_ID, "status": "unknown", "telemetry": None, "telemetry_at": 0,
               "state": None, "rules": None, "brain_hb_at": 0, "broker": False}
events: deque = deque(maxlen=40)
device_log: deque = deque(maxlen=200)
seen_lines: set = set()
log_seq = 0
esp_since = 0
_prev_firing: set = set()
_prev_brain = None


def _load_cleared_seq() -> int:
    try:
        return int(json.loads(CLEAR_PATH.read_text()).get("log_seq", 0))
    except Exception:
        return 0


def _save_cleared_seq(seq: int) -> None:
    try:
        CLEAR_PATH.parent.mkdir(parents=True, exist_ok=True)
        CLEAR_PATH.write_text(json.dumps({"log_seq": seq}))
    except OSError:
        pass


esp_since = _load_cleared_seq()


def ingest_device_line(text: str) -> None:
    global log_seq
    text = text.strip()
    if not text or text in seen_lines:
        return
    seen_lines.add(text)
    log_seq += 1
    device_log.append([log_seq, text])
    if "RULE FIRED" in text:
        log("alarm", text)
    elif "RULE CLEARED" in text:
        log("ok", text)


def log(kind: str, text: str) -> None:
    events.appendleft({"t": time.time(), "kind": kind, "text": text})


def on_connect(c, userdata, flags, rc, props=None):
    with lock:
        model["broker"] = rc == 0
    c.subscribe([(T["telemetry"], 0), (T["status"], 0), (T["state"], 0), ("brain/heartbeat", 0), (T["set"], 0), (T["log"], 0)])
    c.publish("brain/heartbeat", json.dumps({"ts": int(time.time()), "seq": 0}), qos=0)
    log("info", "HMI connected to broker")


def on_disconnect(c, *a):
    with lock:
        model["broker"] = False


def on_message(c, userdata, msg):
    global _prev_firing, _prev_brain, log_seq
    try:
        text = msg.payload.decode()
        with lock:
            if msg.topic == T["log"]:
                ingest_device_line(text)
            elif msg.topic == T["status"]:
                if text != model["status"]:
                    log("ok" if text == "online" else "bad", f"device {DEVICE_ID} {text.upper()}")
                model["status"] = text
            elif msg.topic == "brain/heartbeat":
                model["brain_hb_at"] = time.time()
            elif msg.topic == T["telemetry"]:
                t = json.loads(text)
                model["telemetry"], model["telemetry_at"] = t, time.time()
                b = t.get("brain_online")
                if _prev_brain is not None and b != _prev_brain:
                    log("ok" if b else "bad", "device reports BRAIN ONLINE" if b else "device reports BRAIN OFFLINE — autonomous")
                _prev_brain = b
            elif msg.topic == T["state"]:
                st = json.loads(text)
                model["state"] = st
                _prev_firing = set(st.get("firing", []))
                if st.get("error"):
                    log("bad", f"device rejected rules: {st['error']}")
            elif msg.topic == T["set"]:
                model["rules"] = json.loads(text) if text else []
                n = len(model["rules"])
                log("info", f"rule set pushed ({n} rule(s))")
    except (ValueError, UnicodeDecodeError, TypeError):
        pass


def heartbeat() -> None:
    global esp_since
    seq = 0
    while True:
        if mqtt_client.is_connected():
            seq += 1
            mqtt_client.publish("brain/heartbeat", json.dumps({"ts": int(time.time()), "seq": seq}), qos=0)
        try:
            with urllib.request.urlopen(f"{DEVICE_HTTP}/state?since={esp_since}", timeout=1.2) as r:
                d = json.loads(r.read())
            with lock:
                remote_seq = int(d.get("log_seq", 0))
                if remote_seq < esp_since:
                    esp_since = 0
                for line in d.get("log", []):
                    ingest_device_line(line)
                esp_since = max(esp_since, remote_seq)
        except Exception:
            pass
        time.sleep(1)


def snapshot() -> bytes:
    with lock:
        now = time.time()
        m = dict(model)
        m["telemetry_age"] = round(now - m["telemetry_at"], 1) if m["telemetry_at"] else None
        m["brain_hb_age"] = round(now - m["brain_hb_at"], 1) if m["brain_hb_at"] else None
        m["events"] = list(events)
        m["device_log"] = list(device_log)[-60:]
        m["now"] = now
    return json.dumps(m).encode()


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):  # quiet
        pass

    def _json(self, code: int, obj) -> None:
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("content-type", "application/json")
        self.send_header("cache-control", "no-store")
        self.send_header("content-length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        global esp_since
        # Basic CSRF guard: JSON only, and Origin (when sent) must be this host.
        origin = self.headers.get("origin")
        if origin and origin.split("://", 1)[-1] != self.headers.get("host"):
            return self._json(403, {"error": "cross-origin request refused"})
        if not (self.headers.get("content-type") or "").startswith("application/json"):
            return self._json(415, {"error": "JSON only"})
        n = int(self.headers.get("content-length") or 0)
        if n > 8192:
            return self._json(413, {"error": "too large"})
        try:
            body = json.loads(self.rfile.read(n) or b"{}")
        except ValueError:
            return self._json(400, {"error": "bad JSON"})
        if self.path == "/api/llm":
            try:
                cfg = llmlib.save_config(body)
            except ValueError as e:
                return self._json(400, {"error": str(e)})
            pc = llmlib.public_config(cfg)
            with lock:
                log("info", f"LLM settings saved: {pc['provider']} / {pc['model']}")
            return self._json(200, pc)
        if self.path == "/api/llm/test":
            try:
                return self._json(200, {"ok": True, "message": llmlib.test()})
            except Exception as e:  # noqa: BLE001 — report any failure to the UI
                return self._json(200, {"ok": False, "message": str(e)})
        if self.path in ("/api/rules", "/api/rules/clear"):
            if not isinstance(body, dict):
                return self._json(400, {"error": "expected a JSON object"})
            with lock:
                if not model["broker"]:
                    return self._json(503, {"error": "broker disconnected"})
                state_rules = (model["state"] or {}).get("rules")
                rules = list(state_rules if state_rules is not None else (model["rules"] or []))
            if self.path == "/api/rules/clear":
                rid = body.get("id")
                rules = [r for r in rules if r.get("id") != rid] if rid else []
            else:
                text = body.get("text")
                if not isinstance(text, str) or not text.strip() or len(text) > 500:
                    return self._json(400, {"error": "rule text must be 1–500 characters"})
                if len(rules) >= agentlib.MAX_RULES:
                    return self._json(400, {"error": f"device holds max {agentlib.MAX_RULES} rules"})
                rid = next((f"r{i}" for i in range(1, agentlib.MAX_RULES + 2)
                            if f"r{i}" not in {r.get('id') for r in rules}), None)
                try:
                    rules.append(agentlib.compile_rule(text.strip(), rid))
                except Exception as e:  # noqa: BLE001 — surface compiler/LLM errors in the UI
                    return self._json(400, {"error": str(e)})
            info = mqtt_client.publish(T["set"], json.dumps(rules, separators=(",", ":")), qos=1, retain=True)
            if info.rc != mqtt.MQTT_ERR_SUCCESS:
                return self._json(503, {"error": "could not publish rules to broker"})
            if self.path == "/api/rules/clear" and not body.get("id"):
                mqtt_client.publish(f"edge/{DEVICE_ID}/cmd", json.dumps({"cmd": "reset"}), qos=0)
                time.sleep(0.6)
                try:
                    with urllib.request.urlopen(f"{DEVICE_HTTP}/state?since=0", timeout=1.0) as r:
                        d = json.loads(r.read())
                        esp_since = int(d.get("log_seq", esp_since))
                        with lock:
                            events.clear()
                            device_log.clear()
                            seen_lines.clear()
                            seen_lines.update(s.strip() for s in d.get("log", []) if s.strip())
                            model["rules"] = rules
                            _save_cleared_seq(esp_since)
                except Exception:
                    with lock:
                        events.clear()
                        device_log.clear()
                        seen_lines.clear()
                        model["rules"] = rules
                        _save_cleared_seq(esp_since)
                return self._json(200, {"rules": rules})
            with lock:
                model["rules"] = rules
            return self._json(200, {"rules": rules})
        self.send_error(404)

    def do_GET(self):
        if self.path in ("/", "/index.html"):
            self.send_response(200)
            self.send_header("content-type", "text/html; charset=utf-8")
            self.send_header("content-length", str(len(INDEX)))
            self.end_headers()
            self.wfile.write(INDEX)
        elif self.path == "/api/llm":
            self._json(200, llmlib.public_config())
        elif self.path == "/state":
            body = snapshot()
            self.send_response(200)
            self.send_header("content-type", "application/json")
            self.end_headers()
            self.wfile.write(body)
        elif self.path == "/events":
            self.send_response(200)
            self.send_header("content-type", "text/event-stream")
            self.send_header("cache-control", "no-cache")
            self.end_headers()
            try:
                while True:
                    self.wfile.write(b"data: " + snapshot() + b"\n\n")
                    self.wfile.flush()
                    time.sleep(0.25)
            except (BrokenPipeError, ConnectionResetError):
                return
        else:
            self.send_error(404)


def main() -> None:
    global mqtt_client
    c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"hmi-{os.getpid()}")
    mqtt_client = c
    c.on_connect, c.on_message, c.on_disconnect = on_connect, on_message, on_disconnect
    c.reconnect_delay_set(1, 5)
    c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=10)
    c.loop_start()
    threading.Thread(target=heartbeat, daemon=True).start()
    srv = ThreadingHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    srv.daemon_threads = True
    print(f"HMI on http://0.0.0.0:{HTTP_PORT}  (broker {MQTT_HOST}:{MQTT_PORT}, device {DEVICE_ID})")
    srv.serve_forever()


if __name__ == "__main__":
    main()
