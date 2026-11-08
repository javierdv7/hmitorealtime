#!/usr/bin/env python3
"""Edge Rules agent — plain language -> validated JSON rule -> ESP32-C3 over MQTT.

Runs on the CM4 ("brain"). Publishes brain/heartbeat every 2 s; the device runs the
rules on its own whether or not this process is alive.
"""
from __future__ import annotations

import json
import os
import re
try:
    import readline  # noqa: F401 — line editing + lets us redraw the prompt
except ImportError:
    readline = None
import sys
import threading
import time
from pathlib import Path

import jsonschema
import paho.mqtt.client as mqtt

import llm as llmlib

ROOT = Path(__file__).resolve().parent.parent
SCHEMA_PATH = ROOT / "schema" / "rule.schema.json"

MQTT_HOST = os.environ.get("MQTT_HOST", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", "1883"))
DEVICE_ID = os.environ.get("DEVICE_ID", "edge01")
MAX_RULES = 8


T_RULES_SET = f"edge/{DEVICE_ID}/rules/set"
T_RULES_STATE = f"edge/{DEVICE_ID}/rules/state"
T_TELEMETRY = f"edge/{DEVICE_ID}/telemetry"
T_STATUS = f"edge/{DEVICE_ID}/status"
T_HEARTBEAT = "brain/heartbeat"
T_LOG = f"edge/{DEVICE_ID}/log"

SCHEMA = json.loads(SCHEMA_PATH.read_text())
VALIDATOR = jsonschema.Draft202012Validator(SCHEMA)

# ---------------------------------------------------------------------------- terminal colors
if sys.stdout.isatty() and os.environ.get("NO_COLOR") is None:
    B, DIM, RED, GRN, YEL, CYN, MAG, RST = "\033[1m", "\033[2m", "\033[31m", "\033[32m", "\033[33m", "\033[36m", "\033[35m", "\033[0m"
else:
    B = DIM = RED = GRN = YEL = CYN = MAG = RST = ""


# ---------------------------------------------------------------------------- live output
PROMPT_RAW = f"{B}you ›{RST} "
PROMPT = f"\001{B}\002you ›\001{RST}\002 " if B else "you › "
_print_lock = threading.Lock()
_at_prompt = threading.Event()
show_device_log = True


def live_print(text: str) -> None:
    """Print a line while the user may be typing: clear the prompt line, print, redraw prompt + buffer."""
    with _print_lock:
        if _at_prompt.is_set():
            buf = readline.get_line_buffer() if readline else ""
            sys.stdout.write("\r\033[K" + text + "\n" + PROMPT_RAW + buf)
        else:
            sys.stdout.write(text + "\n")
        sys.stdout.flush()


def color_log(line: str) -> str:
    if re.search(r"FIRED|REJECTED", line):
        c = RED + B
    elif re.search(r"CLEARED|BRAIN ONLINE|CONNECTED|ACCEPTED|LOADED", line):
        c = GRN
    elif re.search(r"OFFLINE|failed|DOWN", line):
        c = YEL + B
    else:
        c = DIM
    return f"  {CYN}◂ {DEVICE_ID}{RST} {c}{line}{RST}"


# ---------------------------------------------------------------------------- MQTT state
class Link:
    def __init__(self) -> None:
        self.lock = threading.Lock()
        self.telemetry: dict | None = None
        self.telemetry_at = 0.0
        self.state: dict | None = None
        self.status = "unknown"
        self.retained_rules: list | None = None
        self.got_retained = threading.Event()
        self.connected = threading.Event()
        self.hb_seq = 0

        self.c = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, client_id=f"brain-agent-{os.getpid()}")
        self.c.on_connect = self._on_connect
        self.c.on_message = self._on_message
        self.c.on_disconnect = lambda *a: self.connected.clear()
        self.c.reconnect_delay_set(1, 5)

    def start(self) -> None:
        self.c.connect_async(MQTT_HOST, MQTT_PORT, keepalive=10)
        self.c.loop_start()
        threading.Thread(target=self._heartbeat, daemon=True).start()

    def _on_connect(self, c, userdata, flags, rc, props=None):
        if rc == 0:
            self.connected.set()
            c.subscribe([(T_TELEMETRY, 0), (T_RULES_STATE, 0), (T_STATUS, 0), (T_RULES_SET, 1), (T_LOG, 0)])

    def _on_message(self, c, userdata, msg):
        try:
            text = msg.payload.decode()
            if msg.topic == T_LOG:
                if show_device_log:
                    live_print(color_log(text))
            elif msg.topic == T_STATUS:
                with self.lock:
                    if text != self.status and self.status != "unknown":
                        live_print(f"  {CYN}◂ {DEVICE_ID}{RST} {GRN if text == 'online' else RED}device {text.upper()} (broker LWT){RST}")
                    self.status = text
            elif msg.topic == T_TELEMETRY:
                with self.lock:
                    self.telemetry = json.loads(text)
                    self.telemetry_at = time.time()
            elif msg.topic == T_RULES_STATE:
                with self.lock:
                    self.state = json.loads(text)
            elif msg.topic == T_RULES_SET:
                if not self.got_retained.is_set():
                    self.retained_rules = json.loads(text) if text else []
                    self.got_retained.set()
        except (ValueError, UnicodeDecodeError):
            pass

    def _heartbeat(self) -> None:
        while True:
            if self.connected.is_set():
                self.hb_seq += 1
                self.c.publish(T_HEARTBEAT, json.dumps({"ts": int(time.time()), "seq": self.hb_seq}), qos=0)
            time.sleep(2)

    def publish_rules(self, rules: list) -> None:
        payload = json.dumps(rules, separators=(",", ":"))
        info = self.c.publish(T_RULES_SET, payload, qos=1, retain=True)
        info.wait_for_publish(timeout=3)


# ---------------------------------------------------------------------------- rule formatting
OP_WORDS = {"==": "is", "!=": "is not", ">": ">", "<": "<", ">=": ">=", "<=": "<="}


def describe(r: dict) -> str:
    inp, v = r["input"], r["value"]
    if inp == "button":
        cond = "button is " + ("PRESSED" if (v == 1) == (r["op"] == "==") else "RELEASED") if r["op"] in ("==", "!=") else f"button {r['op']} {v}"
    elif inp == "temp":
        cond = f"chip temp {OP_WORDS[r['op']]} {v} °C"
    else:
        cond = f"tank level {OP_WORDS[r['op']]} {v} %"
    held = f" for {r['for_ms'] / 1000:g}s" if r["for_ms"] else ""
    a = r["action"]
    act = {"on": "ALARM ON", "off": "ALARM OFF", "blink": f"ALARM BLINK every {a['period_ms']}ms"}[a["type"]]
    latch = " and KEEP IT (latched)" if r["latch"] else ""
    dis = f" {DIM}(disabled){RST}" if not r["enabled"] else ""
    return f"{B}{r['id']}{RST}: WHEN {cond}{held} → {act}{latch}{dis}"


def print_rule(r: dict) -> None:
    print(f"  {GRN}✔ compiled{RST}  {describe(r)}")
    print(f"  {DIM}{json.dumps(r, separators=(',', ':'))}{RST}")


# ---------------------------------------------------------------------------- LLM
SYSTEM_PROMPT = f"""You are a compiler that turns ONE plain-language safety/automation rule into ONE JSON object
for a microcontroller rule engine. Output ONLY the JSON object — no prose, no markdown fences.

JSON Schema (authoritative):
{json.dumps(SCHEMA, indent=1)}

Device facts:
- Output is a single alarm (an LED). "alarm", "light", "LED", "siren", "warning" all mean the output.
- "button" is the physical button: 1 = pressed/held/pushed, 0 = released.
- "temp" is the chip temperature in °C.
- "level" is a simulated tank/process level in percent (0-100). "tank", "level", "fill", "sensor" refer to it.
- "for_ms" is how long the condition must hold continuously. "held for 3 seconds" -> 3000. Immediate -> 0.
- "latch": true when the user says latch / keep it on / stay on / until reset / until cleared.
- action.period_ms: blink period in ms (>= 50) for "blink"; use 0 for "on"/"off". "Blink" with no rate -> 500.
- "while ..." without latch wording -> latch false.
- enabled: true unless the user says otherwise.
- id: use "{{ID}}".
If the request cannot be expressed as one rule, output {{"error": "<short reason>"}}.

Examples:
"if the button is held for 3 seconds, turn on the alarm and latch it" ->
{{"id":"{{ID}}","input":"button","op":"==","value":1,"for_ms":3000,"action":{{"type":"on","period_ms":0}},"latch":true,"enabled":true}}
"blink the alarm every 200ms while the button is pressed" ->
{{"id":"{{ID}}","input":"button","op":"==","value":1,"for_ms":0,"action":{{"type":"blink","period_ms":200}},"latch":false,"enabled":true}}
"""


def llm(messages: list[dict], rule_id: str) -> str:
    return llmlib.chat(SYSTEM_PROMPT.replace("{ID}", rule_id), messages)


def extract_json(text: str):
    text = text.strip()
    text = re.sub(r"^```(?:json)?\s*|\s*```$", "", text)
    try:
        return json.loads(text)
    except ValueError:
        m = re.search(r"\{.*\}", text, re.S)
        if not m:
            raise ValueError("no JSON object in model output")
        return json.loads(m.group(0))


def validation_error(rule) -> str | None:
    errs = sorted(VALIDATOR.iter_errors(rule), key=lambda e: list(e.path))
    if not errs:
        return None
    return "; ".join(f"{'/'.join(map(str, e.path)) or '(root)'}: {e.message}" for e in errs[:5])


def compile_rule(text: str, rule_id: str) -> dict:
    messages = [{"role": "user", "content": text}]
    last_err = ""
    for attempt in (1, 2):
        out = llm(messages, rule_id)
        try:
            rule = extract_json(out)
        except ValueError as e:
            last_err = f"output was not valid JSON ({e})"
        else:
            if isinstance(rule, dict) and "error" in rule and len(rule) == 1:
                raise RuntimeError(f"model says: {rule['error']}")
            if isinstance(rule, dict):
                rule["id"] = rule_id  # ids are owned by the agent
                rule["label"] = text.strip()[:71]  # the sentence, shown on the device page
            last_err = validation_error(rule) or ""
            if not last_err:
                return rule
        if attempt == 1:
            print(f"  {YEL}↻ invalid ({last_err}) — retrying once with the validation error{RST}")
            messages += [
                {"role": "assistant", "content": out},
                {"role": "user", "content": f"That JSON is invalid against the schema: {last_err}. Output the corrected JSON object only."},
            ]
    raise RuntimeError(f"could not compile a valid rule: {last_err}")


# ---------------------------------------------------------------------------- CLI
class Agent:
    def __init__(self, link: Link) -> None:
        self.link = link
        self.rules: list[dict] = []

    def next_id(self) -> str:
        used = {r["id"] for r in self.rules}
        i = 1
        while f"r{i}" in used:
            i += 1
        return f"r{i}"

    def push(self) -> None:
        self.link.publish_rules(self.rules)
        print(f"  {CYN}⇪ pushed {len(self.rules)} rule(s) → {T_RULES_SET} (retained){RST}")
        self.wait_ack()

    def wait_ack(self, timeout: float = 4.0) -> None:
        want = sorted(r["id"] for r in self.rules)
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.link.lock:
                st = self.link.state
            if st:
                if st.get("error"):
                    print(f"  {RED}✖ device rejected rules: {st['error']}{RST}")
                    return
                if sorted(r["id"] for r in st.get("rules", [])) == want and st.get("source") == "mqtt":
                    print(f"  {GRN}✔ device acknowledged — enforcing {len(want)} rule(s) locally{RST}")
                    return
            time.sleep(0.1)
        print(f"  {YEL}… no ack from device yet (offline?). Rule is retained; it will load when the device reconnects.{RST}")

    def status_line(self) -> str:
        with self.link.lock:
            t, at, status = self.link.telemetry, self.link.telemetry_at, self.link.status
        connected = self.link.connected.is_set()
        if not connected:
            return f"{DIM}broker✖ {DEVICE_ID} · sin conexión al servidor{RST}"
        broker = f"{GRN}broker✔{RST}"
        if not t or time.time() - at > 3:
            return f"{broker} device:{RED}{status if status != 'unknown' else 'no data'}{RST}"
        led = f"{RED}{B}ALARM{RST}" if t.get("led") else f"{DIM}alarm off{RST}"
        firing = ",".join(t.get("firing", [])) or "-"
        temp = t.get("temp")
        return (f"{broker} {GRN}{DEVICE_ID}✔{RST} btn={t.get('button')} lvl={t.get('level'):.0f}% "
                f"temp={temp if temp is not None else '?'}°C {led} firing={firing} rules={t.get('rules')} "
                f"loop≤{t.get('loop_max_us')}µs up={t.get('uptime_ms', 0) // 1000}s")

    def cmd_status(self) -> None:
        with self.link.lock:
            t, st, status = self.link.telemetry, self.link.state, self.link.status
        print(f"  device status (LWT): {status}")
        print(f"  {self.status_line()}")
        if t:
            print(f"  brain_online (as seen by device): {GRN if t.get('brain_online') else RED}{t.get('brain_online')}{RST}"
                  f"   rssi={t.get('rssi')}dBm heap={t.get('heap')}B jitter≤{t.get('jitter_max_us')}µs")
        if st:
            print(f"  device rules (source={st.get('source')}): {len(st.get('rules', []))}/{st.get('max')}"
                  + (f"  {RED}last error: {st['error']}{RST}" if st.get("error") else ""))
            for r in st.get("rules", []):
                flag = f"{RED}FIRING{RST}" if r.get("firing") else "idle"
                print(f"    [{flag}] {describe(r)}")

    def cmd_rules(self) -> None:
        if not self.rules:
            print("  (no rules)")
        for r in self.rules:
            print(f"  {describe(r)}")

    def cmd_watch(self) -> None:
        print(f"  {DIM}live telemetry — Ctrl-C to stop{RST}")
        try:
            while True:
                print(f"\r\033[K  {self.status_line()}", end="", flush=True)
                time.sleep(0.25)
        except KeyboardInterrupt:
            print()

    def handle(self, line: str) -> bool:
        line = line.strip()
        if not line:
            return True
        if line in ("/quit", "/exit", "/q"):
            return False
        if line in ("/help", "/?"):
            print(HELP)
        elif line == "/rules":
            self.cmd_rules()
        elif line == "/status":
            self.cmd_status()
        elif line == "/watch":
            self.cmd_watch()
        elif line in ("/log", "/log on", "/log off"):
            global show_device_log
            show_device_log = line != "/log off" and (line == "/log on" or not show_device_log)
            print(f"  live device log {'ON' if show_device_log else 'OFF'}")
        elif line.startswith("/clear"):
            arg = line[len("/clear"):].strip()
            if arg == "all":
                self.rules = []
            elif any(r["id"] == arg for r in self.rules):
                self.rules = [r for r in self.rules if r["id"] != arg]
            else:
                print(f"  usage: /clear <id> | /clear all   (ids: {', '.join(r['id'] for r in self.rules) or '-'})")
                return True
            self.push()
        elif line.startswith("/raw"):
            try:
                rule = json.loads(line[4:])
            except ValueError as e:
                print(f"  {RED}bad JSON: {e}{RST}")
                return True
            err = validation_error(rule)
            if err:
                print(f"  {RED}✖ schema: {err}{RST}")
                return True
            self.add(rule)
        elif line.startswith("/"):
            print("  unknown command — /help")
        else:
            if len(self.rules) >= MAX_RULES:
                print(f"  {RED}device holds max {MAX_RULES} rules — /clear one first{RST}")
                return True
            pc = llmlib.public_config()
            print(f"  {MAG}⚙ compiling with {pc['provider']} ({pc['model']})…{RST}")
            try:
                rule = compile_rule(line, self.next_id())
            except Exception as e:  # noqa: BLE001 — show any failure to the user
                print(f"  {RED}✖ {e}{RST}")
                return True
            self.add(rule)
        return True

    def add(self, rule: dict) -> None:
        if len(self.rules) >= MAX_RULES:
            print(f"  {RED}device holds max {MAX_RULES} rules — /clear one first{RST}")
            return
        if any(r["id"] == rule["id"] for r in self.rules):
            rule["id"] = self.next_id()
        print_rule(rule)
        self.rules.append(rule)
        self.push()


HELP = f"""  Type a rule in plain language, e.g.
    {CYN}if the button is held for 3 seconds, turn on the alarm and latch it{RST}
    {CYN}if the tank level goes above 80% for 1 second, blink the alarm fast{RST}
  Commands:
    /rules          list rules held by the agent
    /clear <id>     remove one rule        /clear all   remove all
    /status         device telemetry, brain_online, rules as the device sees them
    /watch          live status line (Ctrl-C to stop)
    /log [on|off]   live device log in this terminal (default on)
    /raw <json>     push a hand-written rule (validated against the schema)
    /quit"""


def main() -> None:
    link = Link()
    link.start()
    pc = llmlib.public_config()
    key = "" if pc["provider"] != "anthropic" else (" key " + pc["key_hint"] if pc["has_key"] else f" {YEL}(no API key yet — set it in the HMI ⚙ LLM){RST}")
    print(f"{B}EDGE RULES agent{RST} — broker {MQTT_HOST}:{MQTT_PORT}, device {DEVICE_ID}, LLM {pc['provider']}/{pc['model']}{key}")
    if not link.connected.wait(5):
        print(f"{YEL}broker not reachable yet — will keep retrying in background{RST}")
    # Recover the current rule set from the retained message (the broker is the source of truth).
    link.got_retained.wait(1.5)
    agent = Agent(link)
    if isinstance(link.retained_rules, list):
        agent.rules = [r for r in link.retained_rules if isinstance(r, dict) and not validation_error(r)]
        if agent.rules:
            print(f"recovered {len(agent.rules)} rule(s) from retained {T_RULES_SET}")
    print("heartbeat → brain/heartbeat every 2s.  /help for commands.\n")
    time.sleep(1.2)
    while True:
        with _print_lock:
            print(f"{DIM}{agent.status_line()}{RST}")
        _at_prompt.set()
        try:
            line = input(PROMPT)
        except (EOFError, KeyboardInterrupt):
            print()
            break
        finally:
            _at_prompt.clear()
        if not agent.handle(line):
            break


if __name__ == "__main__":
    main()
